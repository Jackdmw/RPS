#define _GNU_SOURCE 
#include "http/rps_http_core.h"
#include "core/rps_conf_file.h"
#include "http/modules/rps_http_proxy_module.h"
#include "http/modules/rps_http_core_module.h"
#include "core/rps_module.h"
#include "core/rps_palloc.h"
#include "core/rps_connection.h"
#include "http/rps_http_phases.h"
#include "core/rps_log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>


static char *rps_conf_set_proxy_pass(rps_conf_t *cf, rps_command_t *cmd, void *conf);
static char *rps_conf_set_proxy_header(rps_conf_t *cf, rps_command_t *cmd, void *conf);

static void *rps_http_proxy_create_loc_conf(rps_conf_t *cf);
static char *rps_http_proxy_merge_loc_conf(rps_pool_t *pool, void *parent, void *child);

static rps_int_t rps_http_proxy_postconfiguration(rps_conf_t *cf);
static rps_int_t rps_http_proxy_handler(rps_http_request_t *r);

static rps_int_t rps_http_proxy_connect_upstream(rps_http_proxy_loc_conf_t *plcf);
static rps_int_t rps_http_proxy_send_request(rps_http_request_t *r,
                                              rps_http_proxy_loc_conf_t *plcf,
                                              int upstream_fd);
static rps_int_t rps_http_proxy_read_response(rps_http_request_t *r, int upstream_fd);


rps_command_t rps_http_proxy_module_commands[] = {
    {
        rps_string("proxy_pass"),
        RPS_HTTP_LOC_CONF | RPS_CONF_TAKE1,
        rps_conf_set_proxy_pass,
        RPS_CONF_BELONG_HTTP_LOC,
        0,
        NULL
    },
    {
        rps_string("proxy_set_header"),
        RPS_HTTP_LOC_CONF | RPS_CONF_TAKE2,
        rps_conf_set_proxy_header,
        RPS_CONF_BELONG_HTTP_LOC,
        0,
        NULL
    },
    {
        rps_string("proxy_connect_timeout"),
        RPS_HTTP_LOC_CONF | RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_HTTP_LOC,
        offsetof(rps_http_proxy_loc_conf_t, connect_timeout),
        NULL
    },
    {
        rps_string("proxy_read_timeout"),
        RPS_HTTP_LOC_CONF | RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_HTTP_LOC,
        offsetof(rps_http_proxy_loc_conf_t, read_timeout),
        NULL
    },
    {
        rps_string("proxy_send_timeout"),
        RPS_HTTP_LOC_CONF | RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_HTTP_LOC,
        offsetof(rps_http_proxy_loc_conf_t, send_timeout),
        NULL
    },
    {
        rps_string("proxy_buffering"),
        RPS_HTTP_LOC_CONF | RPS_CONF_TAKE1,
        rps_conf_set_flag_slot,
        RPS_CONF_BELONG_HTTP_LOC,
        offsetof(rps_http_proxy_loc_conf_t, buffering),
        NULL
    },
    {
        rps_string("proxy_pass_request_headers"),
        RPS_HTTP_LOC_CONF | RPS_CONF_TAKE1,
        rps_conf_set_flag_slot,
        RPS_CONF_BELONG_HTTP_LOC,
        offsetof(rps_http_proxy_loc_conf_t, pass_request_headers),
        NULL
    },
    {
        rps_string("proxy_pass_request_body"),
        RPS_HTTP_LOC_CONF | RPS_CONF_TAKE1,
        rps_conf_set_flag_slot,
        RPS_CONF_BELONG_HTTP_LOC,
        offsetof(rps_http_proxy_loc_conf_t, pass_request_body),
        NULL
    },
    rps_null_command
};


static rps_http_module_t rps_http_proxy_module_ctx = {
    NULL,                               /* preconfiguration */
    rps_http_proxy_postconfiguration,   /* postconfiguration */

    NULL,                               /* create_main_conf */
    NULL,                               /* create_srv_conf */
    rps_http_proxy_create_loc_conf,

    NULL,                               /* merge_srv_conf */
    rps_http_proxy_merge_loc_conf
};

rps_module_t rps_http_proxy_module = {
    -1,
    -1,
    rps_string("http_proxy"),
    "1.0.0",
    &rps_http_proxy_module_ctx,
    rps_http_proxy_module_commands,
    RPS_HTTP_MODULE,
    NULL,   /* init_module   */
    NULL,   /* init_process  */
    NULL,   /* exit_process  */
    NULL    /* exit_master   */
};


static void *
rps_http_proxy_create_loc_conf(rps_conf_t *cf)
{
    rps_http_proxy_loc_conf_t  *plcf;

    plcf = rps_palloc(cf->pool, sizeof(rps_http_proxy_loc_conf_t));
    if (plcf == NULL) {
        return NULL;
    }

    plcf->proxy_pass        = (rps_str_t)rps_null_string;
    plcf->upstream_host     = (rps_str_t)rps_null_string;
    plcf->upstream_port     = RPS_CONF_UNSET_UINT;
    plcf->upstream_uri      = (rps_str_t)rps_null_string;

    plcf->proxy_method      = (rps_str_t)rps_null_string;
    plcf->proxy_http_version = (rps_str_t)rps_null_string;

    if (rps_array_init(&plcf->set_headers, cf->pool, 4,
                       sizeof(rps_http_proxy_header_t)) == RPS_ERROR) {
        return NULL;
    }

    plcf->connect_timeout   = RPS_CONF_UNSET_MSEC;
    plcf->read_timeout      = RPS_CONF_UNSET_MSEC;
    plcf->send_timeout      = RPS_CONF_UNSET_MSEC;

    plcf->buffering              = RPS_CONF_UNSET_UINT;
    plcf->pass_request_headers   = RPS_CONF_UNSET_UINT;
    plcf->pass_request_body      = RPS_CONF_UNSET_UINT;

    return plcf;
}

static char *
rps_http_proxy_merge_loc_conf(rps_pool_t *pool, void *parent, void *child)
{
    rps_http_proxy_loc_conf_t  *prev = parent;
    rps_http_proxy_loc_conf_t  *conf = child;

    if (prev == NULL) {
        return RPS_CONF_OK;
    }

    /* proxy_pass: 子级未配置则继承父级 */
    if (conf->proxy_pass.data == NULL) {
        conf->proxy_pass    = prev->proxy_pass;
        conf->upstream_host = prev->upstream_host;
        conf->upstream_port = prev->upstream_port;
        conf->upstream_uri  = prev->upstream_uri;
    }

    rps_conf_init_value(conf->connect_timeout, prev->connect_timeout);
    rps_conf_init_value(conf->read_timeout,    prev->read_timeout);
    rps_conf_init_value(conf->send_timeout,    prev->send_timeout);

    if (conf->proxy_method.data == NULL) {
        conf->proxy_method = prev->proxy_method;
    }
    if (conf->proxy_http_version.data == NULL) {
        conf->proxy_http_version = prev->proxy_http_version;
    }

    return RPS_CONF_OK;
}


static rps_int_t
rps_http_proxy_postconfiguration(rps_conf_t *cf)
{
    rps_http_conf_container_t  *container;
    rps_http_core_main_conf_t  *cmcf;

    container = cf->ctx;
    cmcf      = container->main_conf[rps_http_core_module.ctx_index];

    rps_http_register_phase_handler(RPS_HTTP_CONTENT_PHASE,
                                     rps_http_proxy_handler,
                                     cmcf);

    return RPS_OK;
}


/*
 * proxy_pass http://backend:8080;
 * proxy_pass http://backend:8080/api;
 * proxy_pass http://backend;
 *
 * 解析 URL 并存储 host / port / uri
 */
static char *
rps_conf_set_proxy_pass(rps_conf_t *cf, rps_command_t *cmd, void *conf)
{
    rps_http_proxy_loc_conf_t  *plcf = conf;
    rps_str_t                  *values;
    rps_str_t                   url;
    u_char                     *p, *start;
    rps_uint_t                  i;

    values = cf->args->elts;
    url    = values[1];

    if (url.len == 0) {
        return "proxy_pass requires a non-empty URL";
    }

    plcf->proxy_pass = url;

    /* 跳过 scheme（http:// 或 https://） */
    p = url.data;
    for (i = 0; i < url.len && p[i] != '\0'; i++) {
        if (p[i] == ':' && i + 2 < url.len && p[i + 1] == '/' && p[i + 2] == '/') {
            p += i + 3;       /* 跳过 "://" */
            break;
        }
    }

    /* host 起点 */
    start = p;

    /* 扫描找 host 结束位置（':' 或 '/' 或 结束） */
    while (p < url.data + url.len && *p != ':' && *p != '/') {
        p++;
    }
    plcf->upstream_host.data = start;
    plcf->upstream_host.len  = (rps_uint_t)(p - start);

    /* 端口 */
    if (p < url.data + url.len && *p == ':') {
        p++; /* 跳过 ':' */
        plcf->upstream_port = 0;
        while (p < url.data + url.len && *p >= '0' && *p <= '9') {
            plcf->upstream_port = plcf->upstream_port * 10 + (rps_uint_t)(*p - '0');
            p++;
        }
    } else {
        plcf->upstream_port = 80;
    }

    /* URI path */
    if (p < url.data + url.len && *p == '/') {
        plcf->upstream_uri.data = p;
        plcf->upstream_uri.len  = (rps_uint_t)(url.data + url.len - p);
    } else {
        plcf->upstream_uri = (rps_str_t)rps_string("/");
    }

    return RPS_CONF_OK;
}

/*
 * proxy_set_header Host $proxy_host;
 * proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
 */
static char *
rps_conf_set_proxy_header(rps_conf_t *cf, rps_command_t *cmd, void *conf)
{
    rps_http_proxy_loc_conf_t  *plcf = conf;
    rps_str_t                  *values;
    rps_http_proxy_header_t    *h;

    values = cf->args->elts;

    h = rps_array_push(&plcf->set_headers);
    if (h == NULL) {
        return "proxy_set_header: array push failed";
    }

    h->key   = values[1];
    h->value = values[2];

    return RPS_CONF_OK;
}


/*
 * CONTENT_PHASE handler：将请求代理到后端 upstream
 *
 * 当 proxy_pass 未配置时返回 RPS_DECLINED
 * 让后续的默认 handler 处理。
 * TODO: 目前的后端连接，采用的是每次处理都要新建连接，后期实现了upstream 模块后，要处理一下。
 */
static rps_int_t
rps_http_proxy_handler(rps_http_request_t *r)
{
    rps_http_proxy_loc_conf_t  *plcf;
    int                         upstream_fd;
    rps_int_t                   rc;

    if (r->loc_conf == NULL) {
        return RPS_DECLINED;
    }
    
    plcf = r->loc_conf[rps_http_proxy_module.ctx_index];
    if (plcf == NULL || plcf->proxy_pass.data == NULL) {
        return RPS_DECLINED;
    }

    /* 填充默认超时值 */
    rps_conf_init_value(plcf->connect_timeout, 60000);
    rps_conf_init_value(plcf->read_timeout,    60000);
    rps_conf_init_value(plcf->send_timeout,    60000);

    /* 填充标志位默认值 */
    rps_conf_init_uint_value(plcf->buffering,            1);
    rps_conf_init_uint_value(plcf->pass_request_headers, 1);
    rps_conf_init_uint_value(plcf->pass_request_body,    1);

    /* 连接到后端 */
    upstream_fd = rps_http_proxy_connect_upstream(plcf);
    if (upstream_fd < 0) {
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        rps_http_send_header(r);
        return RPS_HTTP_DONE;
    }

    /* 转发请求到后端 */
    rc = rps_http_proxy_send_request(r, plcf, upstream_fd);
    if (rc != RPS_OK) {
        close(upstream_fd);
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        rps_http_send_header(r);
        return RPS_HTTP_DONE;
    }

    /* 读取后端响应并转发给客户端 */
    rc = rps_http_proxy_read_response(r, upstream_fd);
    close(upstream_fd);

    if (rc != RPS_OK) {
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        rps_http_send_header(r);
        return RPS_HTTP_DONE;
    }

    return RPS_HTTP_DONE;
}


/*
 * 连接到后端 upstream 服务器（阻塞模式，epoll 完成后改为非阻塞）
 *
 * 使用 getaddrinfo 解析地址，自动处理：
 *   - 域名 → DNS 解析（支持 IPv4 / IPv6 双栈）
 *   - IP 地址 → 直接转换，不走 DNS
 * 
 * TODO:有个问题，每一次处理请求现在都会走一次这个函数调用，而这个函数是在堆中分配的，所以不太好，应该在初始化配置阶段，对所有的proxy创建一个统一的结构体。
 */
static rps_int_t
rps_http_proxy_connect_upstream(rps_http_proxy_loc_conf_t *plcf)
{
    int                     fd;
    struct addrinfo         hints, *res, *rp;
    char                    host_buf[256];
    char                    port_buf[8];

    if (plcf->upstream_host.len >= sizeof(host_buf)) {
        return -1;
    }

    memcpy(host_buf, plcf->upstream_host.data, plcf->upstream_host.len);
    host_buf[plcf->upstream_host.len] = '\0';

    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned int)plcf->upstream_port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;      /* 同时支持 IPv4 和 IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host_buf, port_buf, &hints, &res) != 0) {
        return -1;
    }

    /*
     * 遍历解析结果，尝试连接到第一个成功的地址。
     * 如果 host_buf 是纯 IP，getaddrinfo 直接返回对应的 sockaddr，
     * 不会走 DNS 查询。
     */
    fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  /* 连接成功 */
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    return fd;
}

/*
 * 构造并发送代理请求到后端
 *
 * 请求格式:
 *   GET /path HTTP/1.1\r\n
 *   Host: backend\r\n
 *   Connection: close\r\n
 *   <proxy_set_header 中添加的自定义 header>\r\n
 *   <转发客户端 header>\r\n
 *   \r\n
 */
static rps_int_t
rps_http_proxy_send_request(rps_http_request_t *r,
                             rps_http_proxy_loc_conf_t *plcf,
                             int upstream_fd)
{
    u_char                     *p;
    u_char                      buf[8192];
    rps_http_proxy_header_t    *headers;
    rps_uint_t                  i;
    rps_http_header_kv_t       *hdr;
    ssize_t                     n;

    p = buf;

    /* --- 请求行 --- */
    /* method */
    if (plcf->proxy_method.data != NULL) {
        p = rps_cpymem(p, plcf->proxy_method.data, plcf->proxy_method.len);
    } else {
        p = rps_cpymem(p, r->method.data, r->method.len);
    }
    *p++ = ' ';

    /* URI */
    p = rps_cpymem(p, plcf->upstream_uri.data, plcf->upstream_uri.len);
    if (r->args.len > 0) {
        *p++ = '?';
        p = rps_cpymem(p, r->args.data, r->args.len);
    }
    *p++ = ' ';

    /* HTTP version */
    if (plcf->proxy_http_version.data != NULL) {
        *p++ = 'H'; *p++ = 'T'; *p++ = 'T'; *p++ = 'P'; *p++ = '/';
        p = rps_cpymem(p, plcf->proxy_http_version.data,
                       plcf->proxy_http_version.len);
    } else {
        p = rps_cpymem(p, "HTTP/1.1", 8);
    }
    *p++ = '\r'; *p++ = '\n';

    /* --- Host 头 --- */
    p = rps_cpymem(p, "Host: ", 6);
    p = rps_cpymem(p, plcf->upstream_host.data, plcf->upstream_host.len);
    *p++ = '\r'; *p++ = '\n';

    /* --- proxy_set_header 自定义 header --- */
    headers = plcf->set_headers.elts;
    for (i = 0; i < plcf->set_headers.nelts; i++) {
        p = rps_cpymem(p, headers[i].key.data,   headers[i].key.len);
        *p++ = ':'; *p++ = ' ';
        p = rps_cpymem(p, headers[i].value.data, headers[i].value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* --- 转发客户端固定 header（不与其他 header 重复即可） --- */
    if (plcf->pass_request_headers) {
        if (r->headers_in.user_agent.value.data != NULL) {
            p = rps_cpymem(p, "User-Agent: ", 12);
            p = rps_cpymem(p, r->headers_in.user_agent.value.data,
                           r->headers_in.user_agent.value.len);
            *p++ = '\r'; *p++ = '\n';
        }
        if (r->headers_in.content_type.value.data != NULL) {
            p = rps_cpymem(p, "Content-Type: ", 14);
            p = rps_cpymem(p, r->headers_in.content_type.value.data,
                           r->headers_in.content_type.value.len);
            *p++ = '\r'; *p++ = '\n';
        }
        if (r->headers_in.content_length.value.data != NULL) {
            p = rps_cpymem(p, "Content-Length: ", 16);
            p = rps_cpymem(p, r->headers_in.content_length.value.data,
                           r->headers_in.content_length.value.len);
            *p++ = '\r'; *p++ = '\n';
        }
    }

    /* --- 转发客户端通用 header（rps_list_t 分块迭代） --- */
    if (plcf->pass_request_headers) {
        rps_list_part_t *part;
        part = &r->headers_in.headers.part;
        while (part != NULL) {
            hdr = (rps_http_header_kv_t *)part->elts;
            for (i = 0; i < part->nelts; i++) {
                p = rps_cpymem(p, hdr[i].key.data,   hdr[i].key.len);
                *p++ = ':'; *p++ = ' ';
                p = rps_cpymem(p, hdr[i].value.data, hdr[i].value.len);
                *p++ = '\r'; *p++ = '\n';
            }
            part = part->next;
        }
    }

    /* --- Connection: close（代理模式暂不支持 keepalive 到后端） --- */
    p = rps_cpymem(p, "Connection: close\r\n", 19);

    /* --- 空行结束 header --- */
    *p++ = '\r'; *p++ = '\n';

    n = send(upstream_fd, buf, (size_t)(p - buf), 0);
    if (n < 0) {
        return RPS_ERROR;
    }

    /* --- 转发请求体 --- */
    if (plcf->pass_request_body && r->headers_in.content_length_n > 0) {
        rps_buf_t *body_buf = r->request_body;
        size_t body_len = (size_t)(body_buf->last - body_buf->pos);

        if (body_len > 0) {
            n = send(upstream_fd, body_buf->pos, body_len, 0);
            if (n < 0) {
                return RPS_ERROR;
            }
        }
    }

    return RPS_OK;
}

/*
 * 从后端读取响应，发送给客户端（阻塞模式，epoll 完成后改为非阻塞）
 *
 * 循环 recv 直到读取到完整的 header + body，
 * 然后转发给客户端。
 */
static rps_int_t
rps_http_proxy_read_response(rps_http_request_t *r, int upstream_fd)
{
    u_char      buf[16384];
    u_char     *p, *end, *body_start;
    ssize_t     n, total;

    /* 循环读取直到获得完整数据或对端关闭 */
    total = 0;
    for (;;) {
        n = recv(upstream_fd, buf + total,
                 sizeof(buf) - 1 - (size_t)total, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return RPS_ERROR;
        }
        if (n == 0) {
            break; /* 对端关闭 */
        }
        total += n;
        buf[total] = '\0';

        /* 检查是否已收到完整 header */
        body_start = NULL;
        for (p = buf; p + 3 < buf + total; p++) {
            if (p[0] == '\r' && p[1] == '\n'
                && p[2] == '\r' && p[3] == '\n') {
                body_start = p + 4;
                break;
            }
        }
        if (body_start != NULL) {
            break; /* header 收完 */
        }

        if ((size_t)total >= sizeof(buf) - 1) {
            return RPS_ERROR; /* buffer 溢出 */
        }
    }

    end = buf + total;

    if (body_start == NULL) {
        return RPS_ERROR;
    }

    /* 发送响应头 */
    n = send(r->connection->fd, buf, (size_t)(body_start - buf), 0);
    if (n < 0) {
        return RPS_ERROR;
    }

    /* 发送响应体 */
    {
        size_t body_len = (size_t)(end - body_start);
        if (body_len > 0) {
            n = send(r->connection->fd, body_start, body_len, 0);
            if (n < 0) {
                return RPS_ERROR;
            }
        }
    }

    return RPS_OK;
}
