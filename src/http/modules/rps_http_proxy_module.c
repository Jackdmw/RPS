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
#include <fcntl.h>


static char *rps_conf_set_proxy_pass(rps_conf_t *cf, rps_command_t *cmd, void *conf);
static char *rps_conf_set_proxy_header(rps_conf_t *cf, rps_command_t *cmd, void *conf);

static void *rps_http_proxy_create_loc_conf(rps_conf_t *cf);
static char *rps_http_proxy_merge_loc_conf(rps_pool_t *pool, void *parent, void *child);

static rps_int_t rps_http_proxy_postconfiguration(rps_conf_t *cf);
static rps_int_t rps_http_proxy_handler(rps_http_request_t *r);

/* 非阻塞代理回调 */
static void rps_http_proxy_upstream_write_handler(rps_event_t *ev);
static void rps_http_proxy_upstream_read_handler(rps_event_t *ev);

/* 内部辅助 */
static rps_int_t rps_http_proxy_build_request(rps_http_request_t *r,
    rps_http_proxy_loc_conf_t *plcf, rps_pool_t *pool,
    u_char **buf_out, size_t *len_out);
static void rps_http_proxy_cleanup(rps_http_request_t *r, rps_int_t status);
static rps_int_t rps_http_proxy_connect_upstream(rps_http_proxy_loc_conf_t *plcf);

/*
 * 上游连接上下文，存于 uc->data
 */
typedef struct {
    rps_http_request_t         *r;
    u_char                     *send_buf;
    size_t                      send_len;
    size_t                      send_pos;
} rps_http_upstream_ctx_t;


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
    rps_http_conf_container_t  *parent_container = parent;
    rps_http_conf_container_t  *child_container  = child;
    rps_http_proxy_loc_conf_t  *prev;
    rps_http_proxy_loc_conf_t  *conf;
    rps_http_proxy_header_t    *h;
    rps_uint_t                  i, j;

    i    = rps_http_proxy_module.ctx_index;
    prev = parent_container->loc_conf[i];
    conf = child_container->loc_conf[i];

    if (prev == NULL || conf == NULL) {
        return RPS_CONF_OK;
    }

    if (conf->proxy_pass.data == NULL) {
        conf->proxy_pass    = prev->proxy_pass;
        conf->upstream_host = prev->upstream_host;
        conf->upstream_port = prev->upstream_port;
        conf->upstream_uri  = prev->upstream_uri;
    }

    rps_conf_init_msec_value(conf->connect_timeout, prev->connect_timeout);
    rps_conf_init_msec_value(conf->read_timeout,    prev->read_timeout);
    rps_conf_init_msec_value(conf->send_timeout,    prev->send_timeout);

    /* 设置默认值 */
    rps_conf_init_msec_value(conf->connect_timeout, 60000);
    rps_conf_init_msec_value(conf->read_timeout,    60000);
    rps_conf_init_msec_value(conf->send_timeout,    60000);

    rps_conf_init_uint_value(conf->buffering,            prev->buffering);
    rps_conf_init_uint_value(conf->pass_request_headers, prev->pass_request_headers);
    rps_conf_init_uint_value(conf->pass_request_body,    prev->pass_request_body);

    rps_conf_init_uint_value(conf->buffering,            1);
    rps_conf_init_uint_value(conf->pass_request_headers, 1);
    rps_conf_init_uint_value(conf->pass_request_body,    1);

    if (conf->proxy_method.data == NULL) {
        conf->proxy_method = prev->proxy_method;
    }
    if (conf->proxy_http_version.data == NULL) {
        conf->proxy_http_version = prev->proxy_http_version;
    }

    /* merge set_headers: 父级在前，子级在后（子可覆盖同名 header） */
    if (prev->set_headers.nelts > 0) {
        rps_array_t            merged;
        rps_http_proxy_header_t *ph;

        if (rps_array_init(&merged, pool,
                           prev->set_headers.nelts + conf->set_headers.nelts,
                           sizeof(rps_http_proxy_header_t)) == RPS_ERROR) {
            return "error";
        }

        for (j = 0; j < prev->set_headers.nelts; j++) {
            ph = ((rps_http_proxy_header_t *)prev->set_headers.elts) + j;
            h = rps_array_push(&merged);
            if (h == NULL) return "error";
            h->key = ph->key; h->value = ph->value;
        }
        for (j = 0; j < conf->set_headers.nelts; j++) {
            ph = ((rps_http_proxy_header_t *)conf->set_headers.elts) + j;
            h = rps_array_push(&merged);
            if (h == NULL) return "error";
            h->key = ph->key; h->value = ph->value;
        }

        conf->set_headers = merged;
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

    p = url.data;
    for (i = 0; i < url.len && p[i] != '\0'; i++) {
        if (p[i] == ':' && i + 2 < url.len && p[i + 1] == '/' && p[i + 2] == '/') {
            p += i + 3;
            break;
        }
    }

    start = p;

    while (p < url.data + url.len && *p != ':' && *p != '/') {
        p++;
    }
    plcf->upstream_host.data = start;
    plcf->upstream_host.len  = (rps_uint_t)(p - start);

    if (p < url.data + url.len && *p == ':') {
        p++;
        plcf->upstream_port = 0;
        while (p < url.data + url.len && *p >= '0' && *p <= '9') {
            plcf->upstream_port = plcf->upstream_port * 10 + (rps_uint_t)(*p - '0');
            p++;
        }
    } else {
        plcf->upstream_port = 80;
    }

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
 * 构造要发送到后端的 HTTP 请求（直接写入 buffer，零中间结构）
 */
static rps_int_t
rps_http_proxy_build_request(rps_http_request_t *r,
    rps_http_proxy_loc_conf_t *plcf, rps_pool_t *pool,
    u_char **buf_out, size_t *len_out)
{
    rps_http_proxy_header_t  *ph;
    rps_http_header_kv_t     *hdr;
    rps_list_part_t          *part;
    u_char                   *p, *buf;
    rps_str_t                 ver;
    rps_uint_t                i;

    buf = rps_palloc(pool, 8192);
    if (buf == NULL) return RPS_ERROR;
    p = buf;

    /* ── 请求行 ── */
    if (plcf->proxy_method.data != NULL) {
        p = rps_cpymem(p, plcf->proxy_method.data, plcf->proxy_method.len);
    } else {
        p = rps_cpymem(p, r->method.data, r->method.len);
    }
    *p++ = ' ';

    p = rps_cpymem(p, plcf->upstream_uri.data, plcf->upstream_uri.len);
    if (r->args.len > 0) {
        *p++ = '?';
        p = rps_cpymem(p, r->args.data, r->args.len);
    }
    *p++ = ' ';

    ver = plcf->proxy_http_version;
    if (ver.data != NULL) {
        p = rps_cpymem(p, "HTTP/", 5);
        p = rps_cpymem(p, ver.data, ver.len);
    } else {
        p = rps_cpymem(p, "HTTP/1.1", 8);
    }
    *p++ = '\r'; *p++ = '\n';

    /* ── Host（非 80 带端口）─── */
    p = rps_cpymem(p, "Host: ", 6);
    p = rps_cpymem(p, plcf->upstream_host.data, plcf->upstream_host.len);
    if (plcf->upstream_port != 80) {
        p += snprintf((char *)p, 8, ":%u", (unsigned int)plcf->upstream_port);
    }
    *p++ = '\r'; *p++ = '\n';

    /* ── X-Real-IP / X-Forwarded-For ── */
    if (r->connection->addr_text.data != NULL) {
        p = rps_cpymem(p, "X-Real-IP: ", 11);
        p = rps_cpymem(p, r->connection->addr_text.data,
                       r->connection->addr_text.len);
        *p++ = '\r'; *p++ = '\n';

        p = rps_cpymem(p, "X-Forwarded-For: ", 17);
        p = rps_cpymem(p, r->connection->addr_text.data,
                       r->connection->addr_text.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* ── proxy_set_header ── */
    ph = plcf->set_headers.elts;
    for (i = 0; i < plcf->set_headers.nelts; i++) {
        p = rps_cpymem(p, ph[i].key.data,   ph[i].key.len);
        *p++ = ':'; *p++ = ' ';
        p = rps_cpymem(p, ph[i].value.data, ph[i].value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* ── 转发客户端原始 header ── */
    if (plcf->pass_request_headers) {
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

    /* ── Connection: close ── */
    p = rps_cpymem(p, "Connection: close\r\n", 19);

    /* ── 空行 ── */
    *p++ = '\r'; *p++ = '\n';

    *buf_out = buf;
    *len_out = (size_t)(p - buf);

    return RPS_OK;
}



/*
 * 创建非阻塞 socket 并发起连接到后端。
 * 返回 fd（已设为非阻塞，connect 已发起），失败返回 -1。
 */
static rps_int_t
rps_http_proxy_connect_upstream(rps_http_proxy_loc_conf_t *plcf)
{
    int                     fd;
    struct addrinfo         hints, *res, *rp;
    char                    host_buf[256];
    char                    port_buf[8];
    int                     flags;

    if (plcf->upstream_host.len >= sizeof(host_buf)) {
        return -1;
    }

    memcpy(host_buf, plcf->upstream_host.data, plcf->upstream_host.len);
    host_buf[plcf->upstream_host.len] = '\0';

    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned int)plcf->upstream_port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host_buf, port_buf, &hints, &res) != 0) {
        return -1;
    }

    fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        /* 设为非阻塞 */
        flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            /* 立即连接成功（本地或同一机器） */
            break;
        }
        if (errno == EINPROGRESS) {
            /* 正常的非阻塞连接：fd 已发起，等待可写 */
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    return fd;
}


/*
 * CONTENT_PHASE handler：启动非阻塞代理流程
 */
static rps_int_t
rps_http_proxy_handler(rps_http_request_t *r)
{
    rps_http_proxy_loc_conf_t  *plcf;
    rps_connection_t           *uc;
    rps_http_upstream_ctx_t    *ctx;
    rps_cycle_t                *cycle;
    int                         upstream_fd;
    rps_int_t                   rc;

    if (r->loc_conf == NULL) {
        return RPS_DECLINED;
    }

    plcf = r->loc_conf[rps_http_proxy_module.ctx_index];
    if (plcf == NULL || plcf->proxy_pass.data == NULL) {
        return RPS_DECLINED;
    }

    /* 代理不复用客户端连接，在所有错误路径之前置位 */
    r->keepalive = 0;

    cycle = r->cycle;

    /* 获取一个连接对象用于上游 */
    uc = rps_get_connection(cycle, cycle->log, r->connection->listenling);
    if (uc == NULL) {
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        rps_http_send_header(r);
        return RPS_HTTP_DONE;
    }

    /* 分配上游上下文 */
    ctx = rps_palloc(uc->pool, sizeof(rps_http_upstream_ctx_t));
    if (ctx == NULL) {
        rps_free_connection(uc);
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        rps_http_send_header(r);
        return RPS_HTTP_DONE;
    }

    /* 非阻塞 connect */
    upstream_fd = rps_http_proxy_connect_upstream(plcf);
    if (upstream_fd < 0) {
        rps_free_connection(uc);
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        rps_http_send_header(r);
        return RPS_HTTP_DONE;
    }

    /* 构造要发送的请求 */
    rc = rps_http_proxy_build_request(r, plcf, uc->pool,
                                      &ctx->send_buf, &ctx->send_len);
    if (rc != RPS_OK) {
        close(upstream_fd);
        rps_free_connection(uc);
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        rps_http_send_header(r);
        return RPS_HTTP_DONE;
    }
    ctx->send_pos = 0;

    /* 绑定上下文和连接 */
    ctx->r          = r;
    uc->fd          = upstream_fd;
    uc->data        = ctx;
    r->upstream     = uc;

    /* 设置上游事件回调 */
    uc->write->handler = rps_http_proxy_upstream_write_handler;
    uc->write->data    = uc;
    uc->read->handler  = rps_http_proxy_upstream_read_handler;
    uc->read->data     = uc;

    /* 注册写事件（等待 connect 完成） */
    rc = cycle->event_engine->add_event(uc->write, RPS_WRITE_EVENT);
    if (rc != RPS_OK) {
        /* cleanup 内部已调 finalize + complete_request，checker 不应再处理 */
        rps_http_proxy_cleanup(r, RPS_ERROR);
        return RPS_AGAIN;
    }

    /* connect 超时 */
    rps_event_add_timer(uc->write, plcf->connect_timeout);

    r->proxy_state = 1;

    return RPS_AGAIN;
}


/*
 * 上游 fd 可写回调
 * state 1: connect 完成 → 发送请求
 * state 2: 继续发送（上次 EAGAIN）
 */
static void
rps_http_proxy_upstream_write_handler(rps_event_t *ev)
{
    rps_connection_t           *uc;
    rps_http_upstream_ctx_t    *ctx;
    rps_http_request_t         *r;
    rps_http_proxy_loc_conf_t  *plcf;
    rps_cycle_t                *cycle;
    ssize_t                     n;
    int                         sock_err;
    socklen_t                   sock_err_len;
    rps_int_t                   rc;

    uc  = ev->data;
    ctx = uc->data;
    r   = ctx->r;

    if (r == NULL) {
        return;
    }

    cycle = r->cycle;
    plcf  = r->loc_conf[rps_http_proxy_module.ctx_index];

    /* 超时 */
    if (ev->timedout) {
        rps_log_error(RPS_LOG_ERR, cycle->log, 0,
                      "proxy connect/send timed out");
        rps_http_proxy_cleanup(r, RPS_ERROR);
        return;
    }

    if (r->proxy_state == 1) {
        /* connect 完成：检查 SO_ERROR */
        rps_event_del_timer(ev);

        sock_err_len = sizeof(sock_err);
        if (getsockopt(uc->fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len) < 0
            || sock_err != 0) {
            rps_log_error(RPS_LOG_ERR, cycle->log, sock_err,
                          "proxy connect failed");
            rps_http_proxy_cleanup(r, RPS_ERROR);
            return;
        }

        /* 连接成功：切换到发送状态，设置发送超时 */
        r->proxy_state = 2;
        rps_event_add_timer(ev, plcf->send_timeout);
    }

    if (r->proxy_state == 2) {
        /* 发送请求 */
        n = send(uc->fd, ctx->send_buf + ctx->send_pos,
                 ctx->send_len - ctx->send_pos, 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                /* 保持写事件，等下次可写继续 */
                return;
            }
            rps_log_error(RPS_LOG_ERR, cycle->log, errno,
                          "proxy send failed");
            rps_http_proxy_cleanup(r, RPS_ERROR);
            return;
        }

        ctx->send_pos += (size_t)n;

        if (ctx->send_pos < ctx->send_len) {
            /* 部分发送，保持写事件等待下次 */
            return;
        }

        /* 发送完毕：转发请求体（如果有） */
        if (plcf->pass_request_body && r->headers_in.content_length_n > 0) {
            rps_buf_t *body_buf = r->request_body;
            size_t body_len = (size_t)(body_buf->last - body_buf->pos);

            if (body_len > 0) {
                n = send(uc->fd, body_buf->pos, body_len, 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EINTR) {
                        return;
                    }
                    rps_http_proxy_cleanup(r, RPS_ERROR);
                    return;
                }
                body_buf->pos += n;
                if (body_buf->pos < body_buf->last) {
                    return;
                }
            }
        }

        /* 发送完毕：删除写定时器和写事件，切换到读 */
        rps_event_del_timer(ev);
        r->proxy_state = 3;

        cycle->event_engine->del_event(uc->write, RPS_WRITE_EVENT);

        rc = cycle->event_engine->add_event(uc->read, RPS_READ_EVENT);
        if (rc != RPS_OK) {
            rps_log_error(RPS_LOG_ERR, cycle->log, 0,
                          "failed to add upstream read event");
            rps_http_proxy_cleanup(r, RPS_ERROR);
            return;
        }

        /* 读超时 */
        rps_event_add_timer(uc->read, plcf->read_timeout);
    }
}


/*
 * 上游 fd 可读回调
 * 读取后端响应 → 直接转发给客户端
 */
static void
rps_http_proxy_upstream_read_handler(rps_event_t *ev)
{
    rps_connection_t           *uc;
    rps_http_upstream_ctx_t    *ctx;
    rps_http_request_t         *r;
    rps_connection_t           *c;
    rps_cycle_t                *cycle;
    u_char                      buf[16384];
    ssize_t                     n;

    uc  = ev->data;
    ctx = uc->data;
    r   = ctx->r;

    if (r == NULL) {
        return;
    }

    c     = r->connection;
    cycle = r->cycle;

    /* 超时 */
    if (ev->timedout) {
        rps_log_error(RPS_LOG_ERR, cycle->log, 0,
                      "proxy read timed out");
        rps_http_proxy_cleanup(r, RPS_ERROR);
        return;
    }

    /* 读取上游数据 */
    n = recv(uc->fd, buf, sizeof(buf), 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            return;
        }
        rps_log_error(RPS_LOG_ERR, cycle->log, errno,
                      "proxy recv failed");
        rps_http_proxy_cleanup(r, RPS_ERROR);
        return;
    }

    if (n == 0) {
        /* 上游关闭：转发完毕 */
        rps_event_del_timer(ev);
        cycle->event_engine->del_event(uc->read, RPS_READ_EVENT);

        /* 清除 upstream 引用防止 close_request 二次释放 */
        r->upstream = NULL;

        rps_free_connection(uc);
        rps_http_finalize_request(r, RPS_OK);
        rps_http_complete_request(c);

        return;
    }

    /* 读取到数据：直接转发给客户端 */
    {
        ssize_t sent;
        sent = send(c->fd, buf, (size_t)n, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                /* 客户端写阻塞，丢弃本次数据（简化处理） */
                return;
            }
            rps_http_proxy_cleanup(r, RPS_ERROR);
            return;
        }
        /* 部分写入的数据丢失（简化处理，后续可优化为缓冲重传） */
    }
}


/*
 * 清理代理：关闭上游、释放连接、最终化请求
 */
static void
rps_http_proxy_cleanup(rps_http_request_t *r, rps_int_t status)
{
    rps_connection_t  *uc;
    rps_connection_t  *c;

    c  = r->connection;
    uc = r->upstream;

    /* 清理上游 */
    if (uc != NULL) {
        r->upstream = NULL;

        if (r->cycle != NULL && r->cycle->event_engine != NULL) {
            rps_event_del_timer(uc->write);
            rps_event_del_timer(uc->read);
            r->cycle->event_engine->del_event(uc->write, RPS_WRITE_EVENT);
            r->cycle->event_engine->del_event(uc->read, RPS_READ_EVENT);
        }

        rps_free_connection(uc);
    }

    /* 向客户端发送错误 */
    if (status != RPS_OK && c != NULL) {
        rps_http_finalize_request(r, status);
        rps_http_complete_request(c);
    }
}
