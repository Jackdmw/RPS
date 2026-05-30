#ifndef _RPS_HTTP_CORE_H_INCLUDED_
#define _RPS_HTTP_CORE_H_INCLUDED_

#include "core/rps_config.h"
#include "core/rps_conf_file.h"
#include "core/rps_string.h"

typedef struct rps_http_headers_in_s rps_http_headers_in_t;
typedef struct rps_http_headers_out_s rps_http_headers_out_t;

/*
 * header 键值对，存入通用 header 链表，
 * 用于反向代理时透传客户端/后端的所有 header
 */
typedef struct {
    rps_str_t               key;
    rps_str_t               value;
} rps_http_header_kv_t;

/*
 * 请求头解析结果
 * 固定字段用于高频快速访问（Host 匹配、Connection 判断等），
 * headers 链表存储全部 header，反向代理转发时遍历透传
 */

struct rps_http_headers_in_s{
    /* 固定字段 用于快速访问 */
    rps_http_header_kv_t               host;
    rps_http_header_kv_t               user_agent;
    rps_http_header_kv_t               content_type;
    rps_http_header_kv_t               content_length;
    rps_http_header_kv_t               connection;           /* keep-alive / close */
    rps_uint_t                         content_length_n;     /* Content-Length 解析后的数字 */

    /* 全部 header 链表，元素类型 rps_http_header_kv_t */
    rps_list_t              headers;
    rps_uint_t              headers_n;            /* header 总数 */
};

/*
 * 响应头发送前构造
 * 固定字段在 send_header 时写出，
 * headers 链表存储后端返回的其他 header（如 Set-Cookie、ETag），一并写出
 */
struct rps_http_headers_out_s{
    rps_http_header_kv_t               status;               /* HTTP 状态码，默认 200 */
    rps_http_header_kv_t               content_type;
    rps_http_header_kv_t               server;               /* "RPS" */
    rps_http_header_kv_t               content_length_n;

    rps_list_t              headers;              /* 元素类型 rps_http_header_kv_t */
    rps_uint_t              headers_n;            /* header 总数 */
};

// Buffer 链 — 用于构造响应数据（多个 buf 拼成一次 send）
typedef struct rps_chain_s {
    rps_buf_t              *buf;
    struct rps_chain_s     *next;
} rps_chain_t;

typedef struct {
    void ** main_conf;
    void ** srv_conf;
    void ** loc_conf;
}rps_http_conf_container_t;

typedef struct{
    // 配置解析前后
    rps_int_t (*preconfiguration)(rps_conf_t *cf);
    rps_int_t (*postconfiguration)(rps_conf_t *cf);

    // 三级配置的创建（在解析 http{} 块之前调用）
    void *(*create_main_conf)(rps_conf_t *cf);    // http {} 级
    void *(*create_srv_conf)(rps_conf_t *cf);     // server {} 级
    void *(*create_loc_conf)(rps_conf_t *cf);     // location {} 级

    // 配置合并（上级 → 下级，实现继承）
    char *(*merge_srv_conf)(rps_pool_t *pool, void *parent, void *child);
    char *(*merge_loc_conf)(rps_pool_t *pool, void *parent, void *child);
} rps_http_module_t;


typedef struct rps_http_request_s {
    rps_connection_t       *connection;          /* 客户端连接 */
    rps_cycle_t            *cycle;

    /* 请求行 */
    rps_str_t               method;              /* GET / POST / ... */
    rps_str_t               uri;                 /* /index.html（不含 query string） */
    rps_str_t               args;                /* query string（? 之后的部分） */
    rps_str_t               http_version;        /* HTTP/1.0 或 HTTP/1.1 */
    rps_str_t               host;                /* 请求目标主机，用于虚拟主机匹配 */

    /* 请求头 / 响应头 */
    rps_http_headers_in_t   headers_in;
    rps_http_headers_out_t  headers_out;

    /* 请求体 */
    rps_buf_t              *request_body;        /* 存放已读取的请求数据（含起始行、header、body） */
    size_t                  request_body_rest;   /* 还需读多少字节 body（依据 Content-Length） */
    unsigned                reading_body:1;      /* 是否正在读取请求体 */

    /* 响应体（buffer 链，多个 buf 拼成一次 send） */
    rps_chain_t            *out_chain;

    /*
     * 反向代理相关
     * 代理的完整链路：客户端 ←→ [RPS] ←→ 后端
     * 需要独立的后端连接和缓冲区
     */
    rps_connection_t       *upstream;            /* 到后端 upstream 的连接 */
    rps_buf_t              *upstream_buf;        /* 后端响应数据缓冲区 */
    rps_uint_t              proxy_state;         /* 代理状态机：
                                                   * 0: 空闲
                                                   * 1: 正在连接后端
                                                   * 2: 发送请求到后端
                                                   * 3: 读取后端响应头
                                                   * 4: 读取后端响应体
                                                   * 5: 转发响应给客户端 */

    /* 配置与调度 */
    rps_uint_t              parse_status;        /* 解析阶段标记：
                                                   * 0: 等待解析起始行
                                                   * 1: 等待解析请求头
                                                   * 2: 解析完毕，进入阶段引擎 */
    void                  **main_conf;           /* http{} 级配置指针数组            */
    void                  **srv_conf;            /* server{} 级配置指针数组          */
    void                  **loc_conf;            /* 匹配到的 location 三级配置指针数组 */
    rps_uint_t              phase;               /* 当前阶段（rps_http_phases 枚举值） */
    rps_uint_t              phase_index;         /* 当前 phase_engine handlers[] 的下标 */
    rps_uint_t              internal_redirect;   /* 内部重定向次数（上限 10，防止死循环） */
    unsigned                uri_changed:1;       /* rewrite 阶段是否修改了 URI       */

    /* 生命周期 */
    rps_pool_t             *pool;                /* 请求专属内存池，close_request 时销毁 */
    rps_log_t              *log;
    rps_msec_t              start_msec;          /* 请求到达时间（毫秒），用于超时/日志 */
    unsigned                keepalive:1;         /* 请求完成后是否保持连接（HTTP/1.1 默认 1） */
} rps_http_request_t;




// 创建、销毁
rps_http_request_t *rps_http_create_request(rps_connection_t *c);
void rps_http_close_request(rps_http_request_t *r);
void rps_http_finalize_request(rps_http_request_t *r, rps_int_t rc);
void rps_http_complete_request(rps_connection_t *c);
void rps_http_wait_request_handler(rps_event_t *ev);

// 解析
rps_int_t rps_http_parse_request_line(rps_http_request_t *r);
rps_int_t rps_http_parse_headers(rps_http_request_t *r);

// 发送响应
rps_int_t rps_http_add_response_header(rps_http_request_t *r, rps_str_t key, rps_str_t value);
void      rps_http_set_content_length(rps_http_request_t *r, size_t len);
rps_int_t rps_http_send_header(rps_http_request_t *r);
rps_int_t rps_http_send_body(rps_http_request_t *r, rps_buf_t *body);
rps_int_t rps_http_output_filter(rps_http_request_t *r, rps_chain_t *out);



#endif