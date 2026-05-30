#include "rps_http_core.h"
#include "core/rps_connection.h"
#include "http/rps_http_phases.h"

#define RPS_HTTP_PARSE_STARTLINE                    0
#define RPS_HTTP_PARSE_CRLF                         1
#define RPS_HTTP_PARSE_HEADER                       2
#define RPS_HTTP_PARSE_SPACE                        3
#define RPS_HTTP_PARSE_REQUEST_LINE_METHOD          4
#define RPS_HTTP_PARSE_REQUEST_LINE_URL             5
#define RPS_HTTP_PARSE_STATUS_LINE_VERSION          6
#define RPS_HTTP_PARSE_STATUS_LINE_STATUS           7

#define RPS_HTTP_PARSE_ERROR                        0
#define RPS_HTTP_PARSE_OK                           1
#define RPS_HTTP_PARSE_EAGIN                        2

rps_http_request_t *rps_http_create_request(rps_connection_t *c){
    rps_http_request_t          *request;
    rps_pool_t                  *pool;

    /* 请求独立内存池，与连接池分离，请求结束时只销毁此池 */
    pool = rps_create_pool(4096);
    if (pool == NULL) {
        return NULL;
    }

    request = rps_pcalloc(pool, sizeof(rps_http_request_t));
    if (request == NULL){
        rps_destroy_pool(pool);
        return NULL;
    }
    request -> connection = c;
    request -> pool = pool;
    request -> cycle = c -> cycle;
    /*
     * 请求行字段全部置为空，后续解析起始行时填充
     */
    request -> method = (rps_str_t)rps_null_string;
    request -> uri = (rps_str_t)rps_null_string;
    request -> args = (rps_str_t)rps_null_string;
    request -> http_version = (rps_str_t)rps_null_string;
    request -> host = (rps_str_t)rps_null_string;

    /*
     * 请求头 / 响应头清零，避免脏数据导致误判
     */
    rps_memzero(&request->headers_in, sizeof(rps_http_headers_in_t));
    rps_memzero(&request->headers_out, sizeof(rps_http_headers_out_t));

    request -> headers_in.connection.key = (rps_str_t)rps_string("connection");
    request -> headers_in.content_length.key = (rps_str_t)rps_string("content-length");
    request -> headers_in.user_agent.key = (rps_str_t)rps_string("user-agent");
    request -> headers_in.content_type.key = (rps_str_t)rps_string("content-type");

    request -> headers_out.content_type.key = (rps_str_t)rps_string("content-type");
    request -> headers_out.server.key = (rps_str_t)rps_string("server");
    request -> headers_out.status.key = (rps_str_t)rps_string("status");

    if (rps_list_init(&request -> headers_in.headers, pool, 5, sizeof(rps_http_header_kv_t)) == RPS_ERROR){
        rps_destroy_pool(pool);
        return NULL;
    }
    request -> headers_in.headers_n = 0;
    if (rps_list_init(&request -> headers_out.headers, pool, 5, sizeof(rps_http_header_kv_t)) == RPS_ERROR){
        rps_destroy_pool(pool);
        return NULL;
    }
    request -> headers_out.headers_n = 0;

    /* 默认响应头 */
    request -> headers_out.status.value           = (rps_str_t)rps_string("200 OK");
    request -> headers_out.server.value           = (rps_str_t)rps_string("RPS");
    request -> headers_out.content_type.value     = (rps_str_t)rps_null_string;
    request -> headers_out.content_length_n.value = (rps_str_t)rps_null_string;
    /* 请求体缓冲区：parser 和事件循环需要往里读数据 */
    request->request_body = rps_buf_create(pool, 4096);
    if (request->request_body == NULL) {
        rps_destroy_pool(pool);
        return NULL;
    }
    request->request_body_rest = 0;
    request->reading_body = 0;

    request->out_chain = NULL;

    request->upstream = NULL;
    request->upstream_buf = NULL;
    request->proxy_state = 0;

    request->parse_status = 0;

    request->loc_conf = NULL;

    request->main_conf = NULL;
    request->srv_conf = NULL;
    request->uri_changed = 0;

    /* 阶段引擎从第一个 phase 的第一个 handler 开始 */
    request->phase = RPS_HTTP_POST_READ_PHASE;
    request->phase_index = 0;
    request->internal_redirect = 0;

    request->start_msec = 0; /* TODO: 替换为实际时间戳获取函数 */

    /* HTTP/1.1 默认 keep-alive，解析头部后根据 Connection 头修正 */
    request->keepalive = 1;

    request->log = c->listenling ? c->listenling->log : NULL;

    return request;
}
/**
 * 释放请求的资源（upstream + pool）。
 * 客户端连接本身不在此释放，由外层 done: 统一管理其生命周期。
 */
void rps_http_close_request(rps_http_request_t *r){
    rps_pool_t *pool;

    /* 先存到栈上：r 本身在 pool 中，pool 一旦 free 就不能再读 r 的任何字段 */
    pool = r->pool;

    if (r->upstream) {
        rps_free_connection(r->upstream);
        r->upstream = NULL;
    }

    /* 断开连接与请求的关联 */
    if (r->connection && r->connection->data == r) {
        r->connection->data = NULL;
    }

    if (pool) {
        rps_destroy_pool(pool);
    }
}

/**
 * 本函数用于解析请求行或者状态行
 * 返回值说明：
 * 1. RPS_HTTP_PARSE_EAGIN    缓冲区读完，但未解析完
 * 2. RPS_HTTP_PARSE_ERROR    请求行解析失败
 * 3. RPS_HTTP_PARSE_OK       解析成功 
 */
rps_int_t rps_http_parse_request_line(rps_http_request_t *r){
    rps_buf_t               *body;
    rps_uint_t               i;
    rps_uint_t               j;
    rps_uint_t               status;
    rps_str_t                arg;
    u_char                  *pos;
    char                     c;
    rps_uint_t               num_space = 0;
    
    body = r -> request_body;
    pos = body -> pos;
    status = 0;

    /*找\n 判断是不是一个完整的起始行,以及判断一下是不是两个空格 */
    for (; pos < body -> last; pos ++){
        if (*pos == '\r'){
            if (pos[1] == '\n')
            break;
            else return RPS_HTTP_PARSE_ERROR;
        }
        if (*pos == ' '){
            num_space ++;
            if (num_space == 3){
                return RPS_HTTP_PARSE_ERROR;
            }
        }
    }
    if (pos == body -> last){
        return RPS_HTTP_PARSE_EAGIN;
    }
    
    for (arg.data = body -> pos; body -> pos <= pos; body -> pos ++){
        if (*body -> pos == ' ' || body -> pos == pos){
            arg.len = (rps_uint_t)body -> pos - (rps_uint_t)arg.data;
            status ++;
            if (status == 1){
                if (rps_strcmp_with_cstr(arg, "GET") == RPS_STRING_EQUAL ||
                    rps_strcmp_with_cstr(arg, "POST") == RPS_STRING_EQUAL ||
                    rps_strcmp_with_cstr(arg, "PUT") == RPS_STRING_EQUAL ||
                    rps_strcmp_with_cstr(arg, "HEAD") == RPS_STRING_EQUAL ||
                    rps_strcmp_with_cstr(arg, "DELETE") == RPS_STRING_EQUAL ||
                    rps_strcmp_with_cstr(arg, "CONNECT") == RPS_STRING_EQUAL ||
                    rps_strcmp_with_cstr(arg, "PATCH") == RPS_STRING_EQUAL ||
                    rps_strcmp_with_cstr(arg, "TRACE") == RPS_STRING_EQUAL ||
                    rps_strcmp_with_cstr(arg, "OPTIONS") == RPS_STRING_EQUAL){
                        r -> method = arg;
                    }
            }
            else if (status == 2){
                for (i = 0; i < arg.len; i++){
                    if (arg.data[i] == '?'){
                        r -> args.data = arg.data + i + 1;
                        r -> args.len = arg.len - i - 1;
                        break;
                    }
                }
                r -> uri.data = arg.data;
                r -> uri.len = i;
            }
            else if (status == 3){
                if (rps_strcmp_with_cstr(arg, "HTTP/1.0") == RPS_STRING_EQUAL
                    || rps_strcmp_with_cstr(arg, "HTTP/1.1") == RPS_STRING_EQUAL
                    || rps_strcmp_with_cstr(arg, "HTTP/2.0") == RPS_STRING_EQUAL) {
                    r -> http_version = arg;
                }
            }
            else {
                return RPS_HTTP_PARSE_ERROR;
            }
            arg.data = body -> pos + 1;
        }
    }
    body -> pos = pos + 2;
    r->parse_status = 1;
    return RPS_HTTP_PARSE_OK;
}


/**
 * 返回值说明：
 * 1. RPS_HTTP_PARSE_OK
 * 2. RPS_HTTP_PARSE_ERROR
 * 3. RPS_HTTP_PARSE_EAGAIN
 */
rps_int_t rps_http_parse_headers(rps_http_request_t *r){
    rps_buf_t               *buf;
    u_char                  *pos;
    rps_str_t                key;
    rps_str_t                value;
    rps_uint_t               i;
    rps_uint_t               status;
    rps_http_header_kv_t    *new_header;
    u_char                  *one_header;/*新header行 */
    
    buf = r -> request_body;
    pos = buf -> pos;
    for (one_header = pos; pos < buf -> last; pos ++){
        if (pos[0] == '\r' && pos[1] == '\n'){
            if (one_header == pos){
                buf->pos = pos + 2;
                r->parse_status = 2;

                /* keepalive 判断 */
                if (rps_strcmp_with_cstr(r->http_version, "HTTP/1.0")) {
                    r->keepalive = 0;
                    if (r->headers_in.connection.value.data != NULL
                        && rps_strcmp_with_cstr(r->headers_in.connection.value,
                                                "keep-alive"))
                    {
                        r->keepalive = 1;
                    }
                } else {
                    /* HTTP/1.1 默认 keepalive，Connection: close 时关闭 */
                    r->keepalive = 1;
                    if (r->headers_in.connection.value.data != NULL
                        && rps_strcmp_with_cstr(r->headers_in.connection.value,
                                                "close"))
                    {
                        r->keepalive = 0;
                    }
                }

                return RPS_HTTP_PARSE_OK;
            }

            status = 0;
            key .data = one_header;
            for (i = 0; one_header + i < pos; i++){
                if (one_header[i] == ':'){
                    key.len = i;
                    status = 1;
                    i++;
                    break;
                }
            }
            if (status == 0){
                return RPS_HTTP_PARSE_ERROR;
            }

            while (one_header[i] == ' ')
                i++;
            value.data = one_header + i;
            value.len = (rps_uint_t)pos - (rps_uint_t)(one_header + i);
            
            rps_str_lowercase(key);
            if (rps_strcmp_with_cstr(key, "host") == RPS_STRING_EQUAL){
                r -> headers_in.host.value = value;
            }
            else if (rps_strcmp_with_cstr(key, "user-agent") == RPS_STRING_EQUAL){
                r -> headers_in.user_agent.value = value;
            }
            else if (rps_strcmp_with_cstr(key, "content-type") == RPS_STRING_EQUAL){
                r -> headers_in.content_type.value = value;
            }
            else if (rps_strcmp_with_cstr(key, "content-length") == RPS_STRING_EQUAL){
                r -> headers_in.content_length.value = value;
                r -> headers_in.content_length_n = rps_atoi(value.data,value.len);

                if (r->headers_in.content_length_n == RPS_ERROR){
                    return RPS_HTTP_PARSE_ERROR;
                }
            }
            else if (rps_strcmp_with_cstr(key, "connection") == RPS_STRING_EQUAL){
                r -> headers_in.connection.value =value;
            }
            else {
                new_header = rps_list_push(&r -> headers_in.headers);
                new_header->key = key;
                new_header -> value = value;
            }

            one_header = pos + 2;
        }
    }
    return RPS_HTTP_PARSE_EAGIN;
}