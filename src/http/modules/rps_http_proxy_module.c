#define _GNU_SOURCE
#include "http/rps_http_core.h"
#include "http/rps_upstream.h"
#include "core/rps_conf_file.h"
#include "http/modules/rps_http_proxy_module.h"
#include "http/modules/rps_http_core_module.h"
#include "core/rps_module.h"
#include "core/rps_palloc.h"
#include "core/rps_connection.h"
#include "http/rps_http_phases.h"
#include "core/rps_log.h"
#include "thread/rps_thread.h"

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

/* upstream 协议回调 */
static rps_int_t rps_http_proxy_create_request(rps_http_request_t *r,
                                                rps_upstream_t *u);
static rps_int_t rps_http_proxy_process_response(rps_http_request_t *r,
                                                  rps_upstream_t *u);

/* proxy 专用写事件续传 */
static void rps_http_proxy_write_continue(rps_event_t *ev);


/* WS 双向转发缓冲区 */
#define WS_BUF_SIZE 65536

typedef struct {
    rps_buf_t           *to_upstream;   /* 客户端→后端 待发送缓冲 */
    rps_buf_t           *to_client;     /* 后端→客户端 待发送缓冲 */
    unsigned             client_closed:1;
    unsigned             upstream_closed:1;
} rps_ws_ctx_t;

/* WS upstream 协议回调 */
static rps_int_t ws_create_request(rps_http_request_t *r, rps_upstream_t *u);
static rps_int_t ws_process_response(rps_http_request_t *r, rps_upstream_t *u);

/* WS 双向转发 event handler */
static void ws_client_read_handler(rps_event_t *ev);
static void ws_client_write_handler(rps_event_t *ev);
static void ws_upstream_read_handler(rps_event_t *ev);
static void ws_upstream_write_handler(rps_event_t *ev);

static void ws_close(rps_http_request_t *r);


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
    plcf->upstream_name     = (rps_str_t)rps_null_string;

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
        conf->upstream_name = prev->upstream_name;
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
    rps_str_t                   url, tmp_host, tmp_uri;
    rps_pool_t                 *pool;
    u_char                     *p, *start;
    rps_uint_t                  i;

    pool   = cf->pool;
    values = cf->args->elts;
    url    = values[1];

    if (url.len == 0) {
        return "proxy_pass requires a non-empty URL";
    }

    /* 复制完整 URL 到 pool */
    rps_strcpy(plcf->proxy_pass, url, pool);

    p = url.data;
    /*跳过协议头,http://*/
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
    /* 暂存 host 信息，随后复制到 pool */
    tmp_host.data = start;
    tmp_host.len  = (rps_uint_t)(p - start);
    rps_strcpy(plcf->upstream_host, tmp_host, pool);

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
        /* proxy_pass 带了显式 URI 路径（如 http://backend/api），复制到 pool */
        tmp_uri.data = p;
        tmp_uri.len  = (rps_uint_t)(url.data + url.len - p);
        rps_strcpy(plcf->upstream_uri, tmp_uri, pool);
    } else {
        /* proxy_pass 无路径：保持空串，build_request 时透传原始请求 URI */
        plcf->upstream_uri = (rps_str_t)rps_null_string;
    }

    /*
     * 如果 proxy_pass 是 http://<name> 格式（无端口号、无点号），
     * 预存 upstream_name，在请求时从 upstream {} 块查找
     */
    if (plcf->upstream_port == 80) {
        rps_uint_t has_dot = 0, k;
        for (k = 0; k < plcf->upstream_host.len; k++) {
            if (plcf->upstream_host.data[k] == '.') {
                has_dot = 1; break;
            }
        }
        if (!has_dot) {
            plcf->upstream_name = plcf->upstream_host;
        }
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

    rps_strcpy(h->key,   values[1], cf->pool);
    rps_strcpy(h->value, values[2], cf->pool);
    rps_hash_str_lc(values[1], h->key_hash);

    return RPS_CONF_OK;
}



/*
 * CONTENT_PHASE handler：启动非阻塞代理流程（使用 upstream 层）
 */
static rps_int_t
rps_http_proxy_handler(rps_http_request_t *r)
{
    rps_http_proxy_loc_conf_t  *plcf;
    rps_upstream_t             *u;

    if (r->loc_conf == NULL) return RPS_DECLINED;

    plcf = r->loc_conf[rps_http_proxy_module.ctx_index];
    if (plcf == NULL || plcf->proxy_pass.data == NULL) return RPS_DECLINED;


    u = rps_upstream_create(r);
    if (u == NULL) {
        rps_log_error(RPS_LOG_ALERT, r->log, 0, "proxy: upstream_create FAILED");
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        rps_int_t send_rc = rps_http_send_response(r);
        if (send_rc == RPS_AGAIN) {
            return RPS_AGAIN;
        }
        return RPS_HTTP_DONE;
    }

    /* 设置上游地址 */
    u->peer_addr.host = plcf->upstream_host;
    u->peer_addr.port = plcf->upstream_port;

    /* 如果配置了 upstream_name，尝试从 upstream {} 块查找 */
    if (plcf->upstream_name.data != NULL && plcf->upstream_name.len > 0) {
        rps_http_core_main_conf_t  *cmcf;
        rps_upstream_conf_t       **ups;
        rps_uint_t                  j;

        cmcf = r->main_conf[rps_http_core_module.ctx_index];
        if (cmcf != NULL) {
            ups = cmcf->upstreams.elts;
            for (j = 0; j < cmcf->upstreams.nelts; j++) {
                if (ups[j] != NULL
                    && rps_strcmp(plcf->upstream_name, ups[j]->name)
                       == RPS_STRING_EQUAL)
                {
                    u->upstream_conf = ups[j];
                    break;
                }
            }
        }
    }

    /* 超时 */
    u->connect_timeout = plcf->connect_timeout;
    u->send_timeout    = plcf->send_timeout;
    u->read_timeout    = plcf->read_timeout;

    /* TEMP: disable WS detection, force HTTP proxy path */
    /*
     * 检测 WebSocket 升级请求：Upgrade: websocket + Connection 含 "upgrade"
     * 命中则走 WS 透明代理路径，否则走标准 HTTP 代理。
     */
    {
        unsigned is_ws = 0;

        if (r->headers_in.upgrade.value.data != NULL
            && r->headers_in.connection.value.data != NULL)
        {
            rps_str_lowercase(r->headers_in.upgrade.value);
            if (rps_strcmp_with_cstr(r->headers_in.upgrade.value, "websocket"))
            {
                rps_str_lowercase(r->headers_in.connection.value);
                if (r->headers_in.connection.value.len >= 7)
                {
                    u_char *p = r->headers_in.connection.value.data;
                    u_char *end = p + r->headers_in.connection.value.len - 6;
                    for (; p <= end; p++) {
                        if (memcmp(p, "upgrade", 7) == 0) { is_ws = 1; break; }
                    }
                }
            }
        }

        if (is_ws) {
            rps_log_error(RPS_LOG_INFO, r -> cycle -> log, 0, "HTTP Request for websocket");
            u->create_request   = ws_create_request;
            u->process_response = ws_process_response;
            u->write_continue   = rps_http_proxy_write_continue;
        } else {
            rps_log_error(RPS_LOG_INFO, r -> cycle -> log, 0, "Normal HTTP Request");
            u->create_request   = rps_http_proxy_create_request;
            u->process_response = rps_http_proxy_process_response;
            u->write_continue   = rps_http_proxy_write_continue;
        }
    }

    r->upstream = u;

    if (r->cycle->if_pthread) {
        /*
         * 线程模式：阻塞执行 upstream，不走事件驱动。
         * 返回 RPS_AGAIN 阻止阶段引擎 auto-finalize——线程 worker
         * 在 run_phases 返回后自行管理 keepalive 循环和 finalize。
         */
        if (u->create_request == ws_create_request) {
            rps_thread_ws_start(r, u);
        } else {
            rps_thread_proxy_run(r, u);
        }
        return RPS_AGAIN;
    }

    /* 启动 upstream。失败时 upstream_finalize 自动发送错误响应 + 清理 */
    rps_upstream_init(r, u);
    return RPS_AGAIN;
}

/*
 * upstream 回调：构造发往后端的 HTTP 请求（直接写入 u->request_bufs）
 */
static rps_int_t
rps_http_proxy_create_request(rps_http_request_t *r, rps_upstream_t *u)
{
    rps_http_proxy_loc_conf_t  *plcf;
    rps_http_proxy_header_t    *ph;
    rps_http_header_kv_t       *hdr;
    rps_list_part_t            *part;
    u_char                     *p;
    rps_str_t                   ver;
    rps_uint_t                  i;

    plcf = r->loc_conf[rps_http_proxy_module.ctx_index];

    u->request_bufs = rps_buf_create(r->pool, 8192);
    if (u->request_bufs == NULL) return RPS_ERROR;

    p = u->request_bufs->pos;

    /* ── 请求行 ── */
    if (plcf->proxy_method.data != NULL) {
        p = rps_cpymem(p, plcf->proxy_method.data, plcf->proxy_method.len);
    } else {
        p = rps_cpymem(p, r->method.data, r->method.len);
    }
    *p++ = ' ';

    if (plcf->upstream_uri.data != NULL && plcf->upstream_uri.len > 0) {
        p = rps_cpymem(p, plcf->upstream_uri.data, plcf->upstream_uri.len);
    } else {
        p = rps_cpymem(p, r->uri.data, r->uri.len);
    }
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

    /* ── Host ── */
    p = rps_cpymem(p, "Host: ", 6);
    if (plcf -> upstream_name.data){
        p = rps_cpymem(p, u -> peer_addr.host.data, u->peer_addr.host.len);
        *p++ = ':';
        p += sprintf(p,"%lu",u->peer_addr.port);
    }
    else{
        p = rps_cpymem(p, plcf->upstream_host.data, plcf->upstream_host.len);
    }
    if (plcf->upstream_port != 80) {
        p += snprintf((char *)p, 8, ":%u", (unsigned int)plcf->upstream_port);
    }
    *p++ = '\r'; *p++ = '\n';

    /* ── X-Real-IP / X-Forwarded-For ──
     * X-Real-IP：客户端已设置则保留（多层代理透传），否则写入直连 IP
     * X-Forwarded-For：追加当前客户端 IP，而非覆盖
     */
    if (r->connection->addr_text.data != NULL) {
        unsigned    has_real_ip  = 0;
        unsigned    has_xff      = 0;
        rps_str_t   xff_orig     = {0, NULL};

        /* 扫描客户端原始头中是否已有 X-Real-IP / X-Forwarded-For */
        {
            rps_http_header_kv_t *sh;
            rps_list_part_t      *sp;
            rps_uint_t            k;

            sp = &r->headers_in.headers.part;
            while (sp != NULL) {
                sh = (rps_http_header_kv_t *)sp->elts;
                for (k = 0; k < sp->nelts; k++) {
                    if (!has_real_ip
                        && rps_strcmp_with_cstr(sh[k].key, "x-real-ip"))
                    {
                        has_real_ip = 1;
                    }
                    if (!has_xff
                        && rps_strcmp_with_cstr(sh[k].key, "x-forwarded-for"))
                    {
                        has_xff = 1;
                        xff_orig = sh[k].value;
                    }
                    if (has_real_ip && has_xff) goto xff_done;
                }
                sp = sp->next;
            }
            xff_done:;
        }

        /* X-Real-IP：仅当上游未设置时才写入 */
        if (!has_real_ip) {
            p = rps_cpymem(p, "X-Real-IP: ", 11);
            p = rps_cpymem(p, r->connection->addr_text.data,
                           r->connection->addr_text.len);
            *p++ = '\r'; *p++ = '\n';
        }

        /* X-Forwarded-For：追加当前客户端 IP */
        p = rps_cpymem(p, "X-Forwarded-For: ", 17);
        if (has_xff && xff_orig.data != NULL) {
            p = rps_cpymem(p, xff_orig.data, xff_orig.len);
            *p++ = ','; *p++ = ' ';
        }
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

    /* ── 转发客户端原始 header（去重：跳过已显式设置的头）── */
    if (plcf->pass_request_headers) {
        part = &r->headers_in.headers.part;
        while (part != NULL) {
            hdr = (rps_http_header_kv_t *)part->elts;
            for (i = 0; i < part->nelts; i++) {
                rps_uint_t  j;
                rps_uint_t  h;
                unsigned    skip = 0;

                /* 跳过已写内置头（host / connection / x-forwarded-for 总是自己构造） */
                if (rps_strcmp_with_cstr(hdr[i].key, "host")
                    || rps_strcmp_with_cstr(hdr[i].key, "connection")
                    || rps_strcmp_with_cstr(hdr[i].key, "x-forwarded-for"))
                {
                    continue;
                }

                /* 跳过 proxy_set_header 已覆盖的头（hash 加速比对） */
                rps_hash_str_lc(hdr[i].key, h);
                for (j = 0; j < plcf->set_headers.nelts; j++) {
                    if (h == ph[j].key_hash
                        && rps_strcmp(hdr[i].key, ph[j].key))
                    {
                        skip = 1; break;
                    }
                }
                if (skip) continue;

                p = rps_cpymem(p, hdr[i].key.data,   hdr[i].key.len);
                *p++ = ':'; *p++ = ' ';
                p = rps_cpymem(p, hdr[i].value.data, hdr[i].value.len);
                *p++ = '\r'; *p++ = '\n';
            }
            part = part->next;
        }
    }

    /* ── Connection ── */
    if (u->upstream_conf != NULL && u->upstream_conf->keepalive > 0) {
        p = rps_cpymem(p, "Connection: keep-alive\r\n", 24);
    } else {
        p = rps_cpymem(p, "Connection: close\r\n", 19);
    }

    /* ── 空行 ── */
    *p++ = '\r'; *p++ = '\n';
    if (r ->request_body_pos != r -> request_body->last){
        p = rps_cpymem(p,r -> request_body_pos, r -> headers_in.content_length_n);
    }
    u->request_bufs->last = p;

    return RPS_OK;
}

/*
 * upstream 回调：解析后端 HTTP 响应头
 *
 * 从 u->response_buf 中解析后端的 status line 和 headers，
 * 填充 r->headers_out，然后发送给客户端。
 *
 * 返回 RPS_OK     — 响应头已成功发送
 * 返回 RPS_AGAIN  — 数据不足（等下次读）或写阻塞（挂起续传）
 * 返回 RPS_ERROR  — 解析 / 发送失败
 */
static rps_int_t
rps_http_proxy_process_response(rps_http_request_t *r, rps_upstream_t *u)
{
    u_char   *p, *end, *line, *header_end;
    rps_int_t wrc;

    p   = u->response_buf->pos;
    end = u->response_buf->last;

    /* 找 \r\n\r\n 标记头部结束 */
    header_end = NULL;
    {
        u_char *s = p;
        while (s + 3 < end) {
            if (s[0] == '\r' && s[1] == '\n'
                && s[2] == '\r' && s[3] == '\n') {
                header_end = s;
                break;
            }
            s++;
        }
    }

    if (header_end == NULL) {
        return RPS_AGAIN;   /* 头部不完整，等更多数据 */
    }

    /* ── 解析 status line：HTTP/x.x → 定 keepalive 默认值 ── */
    line = p;
    {
        u_char *status_cr = line;
        u_char *sp;
        while (status_cr < header_end && *status_cr != '\r') status_cr++;
        if (status_cr >= header_end) return RPS_ERROR;

        /* 提取 HTTP 版本，决定 keepalive 默认策略 */
        sp = line;
        while (sp < status_cr && *sp != ' ') sp++;

        if (sp - line >= 8) {
            /* HTTP/1.1 默认 keep-alive, HTTP/1.0 默认 close */
            u->keepalive = (line[5] == '1' && line[6] == '.' && line[7] == '1') ? 1 : 0;
        }

        if (sp < status_cr) {
            sp++;  /* 跳过第一个空格 */
            r->headers_out.status.value.data = sp;
            r->headers_out.status.value.len  = (rps_uint_t)(status_cr - sp);
        }

        /* 跳过 status line 的 \r\n */
        p = status_cr + 2;
    }

    /* ── 逐行解析响应头 ── */
    while (p < header_end) {
        u_char *cr = p;
        while (cr < header_end && *cr != '\r') cr++;
        if (cr >= header_end) break;

        /* 找冒号分隔 key: value */
        {
            u_char *colon = p;
            while (colon < cr && *colon != ':') colon++;
            if (colon < cr) {
                rps_str_t key, value;
                key.data = p;
                key.len  = (rps_uint_t)(colon - p);

                /* 跳过冒号和空白 */
                colon++;
                while (colon < cr && *colon == ' ') colon++;
                value.data = colon;
                value.len  = (rps_uint_t)(cr - colon);

                rps_str_lowercase(key);

                /* 填入固定字段，加速后续访问 */
                if (rps_strcmp_with_cstr(key, "content-type")) {
                    r->headers_out.content_type.value = value;
                } else if (rps_strcmp_with_cstr(key, "content-length")) {
                    r->headers_out.content_length_n.value = value;
                    u->content_length_n = (size_t)rps_atoi(value.data, value.len);
                    rps_http_set_content_length(r, u->content_length_n);
                } else if (rps_strcmp_with_cstr(key, "server")) {
                    r->headers_out.server.value = value;
                } else if (rps_strcmp_with_cstr(key, "connection")) {
                    /* 后端 Connection 头决定此连接是否可 keepalive 复用 */
                    rps_str_lowercase(value);
                    if (value.len == 5 && memcmp(value.data, "close", 5) == 0) {
                        u->keepalive = 0;
                    } else if (value.len == 10 && memcmp(value.data, "keep-alive", 10) == 0) {
                        u->keepalive = 1;
                    }
                } else {
                    /* 透传其他 header（Set-Cookie, ETag 等） */
                    rps_http_add_response_header(r, key, value);
                }
            }
        }

        p = cr + 2;  /* 跳过 \r\n */
    }

    /* 移动到 body 起始，记录初始 body 字节数 */
    u->response_buf->pos = header_end + 4;
    u->body_received = (size_t)(u->response_buf->last - u->response_buf->pos);
    u->header_complete = 1;
    u->headers_sent    = 1;
    u->read_state = RPS_UPSTREAM_READ_BODY;
    /*
     * 先构造 header chain（header_filter），再拼 body chain，
     * 最后统一 write_filter 发送。避免 send_header EAGAIN 时
     * body chain 未挂载导致 body 数据丢失。
     */
    if (rps_http_header_filter(r) != RPS_OK) return RPS_ERROR;

    {
        rps_chain_t *cl = rps_palloc(r->pool, sizeof(rps_chain_t));
        rps_chain_t *last;
        if (cl == NULL) return RPS_ERROR;
        cl->buf  = u->response_buf;
        cl->next = NULL;
        /* body chain 追加到末尾（header_filter 已把 header 插在头部） */
        if (r->out_chain == NULL) {
            r->out_chain = cl;
        } else {
            for (last = r->out_chain; last->next != NULL; last = last->next);
            last->next = cl;
        }
    }

    {
        rps_int_t brc = rps_http_write_filter(r);
        if (brc == RPS_AGAIN) {
            if (u->peer && u->peer->read && u->peer->read->active)
                r->cycle->event_engine->del_event(u->peer->read, RPS_READ_EVENT);
            r->connection->write->handler = u->write_continue;
            return RPS_AGAIN;
        }
        if (brc != RPS_OK) return RPS_ERROR;
    }

    return RPS_OK;
}


/*
 * proxy 专用写事件续传回调。
 *
 * rps_http_send_header → write_filter 在 EAGAIN 时默认注册
 * rps_http_write_filter_continue（会 finalize request），
 * 但 proxy 路径下 request 仍被 upstream 引用，不可销毁。
 * 此回调替代之：发送完毕后恢复 upstream 读取，仅在出错时 finalize。
 */
static void
rps_http_proxy_write_continue(rps_event_t *ev)
{
    rps_http_request_t *r;
    rps_upstream_t     *u;
    rps_connection_t   *c;
    rps_int_t           rc;

    c = ev->connection;
    r = ev->data;
    if (c == NULL || r == NULL || r->upstream == NULL) return;
    u = r->upstream;

    if (ev->timedout) {
        rps_log_error(RPS_LOG_ERR, ev -> log, 0, "send to client timedout");
        rps_upstream_finalize(r, RPS_ERROR);
        return;
    }

    rc = rps_http_write_filter(r);

    if (rc == RPS_OK) {
        /* 写完成：清理写事件 */
        r->cycle->event_engine->del_event(c->write, RPS_WRITE_EVENT);
        c->write->active = 0;
        rps_event_del_timer(ev);

        /* buf 中滞留数据已发完，复位腾空间 */
        if (u->response_buf->pos == u->response_buf->last) {
            u->response_buf->pos  = u->response_buf->start;
            u->response_buf->last = u->response_buf->start;
        }

        /* response 已完成 → 结束 */
        if ((!u->peer || !u->peer->read || !u->peer->read->active)) {
            rps_upstream_finalize(r, RPS_OK);
            return;
        }

        /* 恢复后端读，等更多 body */
        if (r->cycle->event_engine->add_event(u->peer->read,
                                              RPS_READ_EVENT) != RPS_OK) {
            rps_upstream_finalize(r, RPS_ERROR);
            return;
        }
        rps_event_add_timer(u->peer->read, u->read_timeout);
    } else if (rc == RPS_ERROR) {
        rps_upstream_finalize(r, RPS_ERROR);
    } else {
        c->write->handler = rps_http_proxy_write_continue;
    }
}



/*

/* 
 * WebSocket 代理实现 (Phase 1: 透明转发)
 */

/*
 * upstream 回调：构造 WebSocket 升级请求
 * 透传 Upgrade / Connection / Sec-WebSocket-Key / Version，其余同 HTTP 代理
 */
static rps_int_t
ws_create_request(rps_http_request_t *r, rps_upstream_t *u)
{
    rps_http_proxy_loc_conf_t  *plcf;
    rps_http_proxy_header_t    *ph;
    rps_http_header_kv_t       *hdr;
    rps_list_part_t            *part;
    u_char                     *p;
    rps_str_t                   ver;
    rps_uint_t                  i;

    plcf = r->loc_conf[rps_http_proxy_module.ctx_index];

    u->request_bufs = rps_buf_create(r->pool, 8192);
    if (u->request_bufs == NULL) return RPS_ERROR;

    p = u->request_bufs->pos;

    /* ── 请求行 ── */
    if (plcf->proxy_method.data != NULL) {
        p = rps_cpymem(p, plcf->proxy_method.data, plcf->proxy_method.len);
    } else {
        p = rps_cpymem(p, r->method.data, r->method.len);
    }
    *p++ = ' ';

    if (plcf->upstream_uri.data != NULL && plcf->upstream_uri.len > 0) {
        p = rps_cpymem(p, plcf->upstream_uri.data, plcf->upstream_uri.len);
    } else {
        p = rps_cpymem(p, r->uri.data, r->uri.len);
    }
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

    /* ── Host ── */
    p = rps_cpymem(p, "Host: ", 6);
    p = rps_cpymem(p, plcf->upstream_host.data, plcf->upstream_host.len);
    if (plcf->upstream_port != 80) {
        p += snprintf((char *)p, 8, ":%u", (unsigned int)plcf->upstream_port);
    }
    *p++ = '\r'; *p++ = '\n';

    /* ── X-Real-IP / X-Forwarded-For ── */
    if (r->connection->addr_text.data != NULL) {
        unsigned    has_real_ip  = 0;
        unsigned    has_xff      = 0;
        rps_str_t   xff_orig     = {0, NULL};

        rps_http_header_kv_t *sh;
        rps_list_part_t      *sp;
        rps_uint_t            k;

        sp = &r->headers_in.headers.part;
        while (sp != NULL) {
            sh = (rps_http_header_kv_t *)sp->elts;
            for (k = 0; k < sp->nelts; k++) {
                if (!has_real_ip
                    && rps_strcmp_with_cstr(sh[k].key, "x-real-ip"))
                    { has_real_ip = 1; }
                if (!has_xff
                    && rps_strcmp_with_cstr(sh[k].key, "x-forwarded-for"))
                    { has_xff = 1; xff_orig = sh[k].value; }
                if (has_real_ip && has_xff) goto ws_xff_done;
            }
            sp = sp->next;
        }
        ws_xff_done:;

        if (!has_real_ip) {
            p = rps_cpymem(p, "X-Real-IP: ", 11);
            p = rps_cpymem(p, r->connection->addr_text.data,
                           r->connection->addr_text.len);
            *p++ = '\r'; *p++ = '\n';
        }

        p = rps_cpymem(p, "X-Forwarded-For: ", 17);
        if (has_xff && xff_orig.data != NULL) {
            p = rps_cpymem(p, xff_orig.data, xff_orig.len);
            *p++ = ','; *p++ = ' ';
        }
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

    /* ── 透传 WS 关键头：Upgrade / Connection / Key / Version ── */
    if (r->headers_in.upgrade.value.data != NULL) {
        p = rps_cpymem(p, "Upgrade: ", 9);
        p = rps_cpymem(p, r->headers_in.upgrade.value.data,
                       r->headers_in.upgrade.value.len);
        *p++ = '\r'; *p++ = '\n';
    }
    p = rps_cpymem(p, "Connection: Upgrade\r\n", 21);

    /* ── Sec-WebSocket-Key / Version ── */
    if (r->headers_in.sec_websocket_key.value.data != NULL) {
        p = rps_cpymem(p, "Sec-WebSocket-Key: ", 19);
        p = rps_cpymem(p, r->headers_in.sec_websocket_key.value.data,
                       r->headers_in.sec_websocket_key.value.len);
        *p++ = '\r'; *p++ = '\n';
    }
    if (r->headers_in.sec_websocket_version.value.data != NULL) {
        p = rps_cpymem(p, "Sec-WebSocket-Version: ", 23);
        p = rps_cpymem(p, r->headers_in.sec_websocket_version.value.data,
                       r->headers_in.sec_websocket_version.value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* ── 透传 Sec-WebSocket-Protocol / Extensions (如果存在) ── */
    {
        rps_http_header_kv_t *sh;
        rps_list_part_t      *sp;
        rps_uint_t            k;

        sp = &r->headers_in.headers.part;
        while (sp != NULL) {
            sh = (rps_http_header_kv_t *)sp->elts;
            for (k = 0; k < sp->nelts; k++) {
                if (rps_strcmp_with_cstr(sh[k].key, "sec-websocket-protocol"))
                {
                    p = rps_cpymem(p, "Sec-WebSocket-Protocol: ", 24);
                    p = rps_cpymem(p, sh[k].value.data, sh[k].value.len);
                    *p++ = '\r'; *p++ = '\n';
                }
                if (rps_strcmp_with_cstr(sh[k].key, "sec-websocket-extensions"))
                {
                    p = rps_cpymem(p, "Sec-WebSocket-Extensions: ", 26);
                    p = rps_cpymem(p, sh[k].value.data, sh[k].value.len);
                    *p++ = '\r'; *p++ = '\n';
                }
            }
            sp = sp->next;
        }
    }

    /* ── 转发其他客户端 header（去重）── */
    if (plcf->pass_request_headers) {
        part = &r->headers_in.headers.part;
        while (part != NULL) {
            hdr = (rps_http_header_kv_t *)part->elts;
            for (i = 0; i < part->nelts; i++) {
                rps_uint_t  j;
                rps_uint_t  h;
                unsigned    skip = 0;

                if (rps_strcmp_with_cstr(hdr[i].key, "host")
                    || rps_strcmp_with_cstr(hdr[i].key, "connection")
                    || rps_strcmp_with_cstr(hdr[i].key, "x-forwarded-for")
                    || rps_strcmp_with_cstr(hdr[i].key, "upgrade")
                    || rps_strcmp_with_cstr(hdr[i].key, "sec-websocket-key")
                    || rps_strcmp_with_cstr(hdr[i].key, "sec-websocket-version")
                    || rps_strcmp_with_cstr(hdr[i].key, "sec-websocket-protocol")
                    || rps_strcmp_with_cstr(hdr[i].key, "sec-websocket-extensions"))
                    { continue; }

                rps_hash_str_lc(hdr[i].key, h);
                for (j = 0; j < plcf->set_headers.nelts; j++) {
                    if (h == ph[j].key_hash
                        && rps_strcmp(hdr[i].key, ph[j].key))
                        { skip = 1; break; }
                }
                if (skip) continue;

                p = rps_cpymem(p, hdr[i].key.data,   hdr[i].key.len);
                *p++ = ':'; *p++ = ' ';
                p = rps_cpymem(p, hdr[i].value.data, hdr[i].value.len);
                *p++ = '\r'; *p++ = '\n';
            }
            part = part->next;
        }
    }

    /* ── 空行 ── */
    *p++ = '\r'; *p++ = '\n';

    u->request_bufs->last = p;
    return RPS_OK;
}

/*
 * upstream 回调：解析后端 101 响应，透传给客户端，切换为双向转发模式
 */
static rps_int_t
ws_process_response(rps_http_request_t *r, rps_upstream_t *u)
{
    u_char   *p, *end, *header_end;

    p   = u->response_buf->pos;
    end = u->response_buf->last;

    /* 找 \r\n\r\n */
    header_end = NULL;
    {
        u_char *s = p;
        while (s + 3 < end) {
            if (s[0] == '\r' && s[1] == '\n'
                && s[2] == '\r' && s[3] == '\n') {
                header_end = s;
                break;
            }
            s++;
        }
    }

    if (header_end == NULL) return RPS_AGAIN;

    /* ── 校验 101 状态码 ── */
    {
        u_char *cr = p;
        while (cr < header_end && *cr != '\r') cr++;

        /* 跳过 "HTTP/x.x "，定位状态码 */
        u_char *sp = p;
        while (sp < cr && *sp != ' ') sp++;
        if (sp < cr) sp++;
        if (!(sp + 3 <= cr && sp[0] == '1' && sp[1] == '0' && sp[2] == '1'))
        {
            rps_log_error(RPS_LOG_ERR, r->log, 0, "WS: backend did not return 101");
            return RPS_ERROR;
        }
    }

    /*
     * 101 响应直接透传后端原始数据，不过 header_filter。
     * WS 握手响应必须原样转发（Upgrade / Connection / Sec-WebSocket-Accept）。
     */
    {
        u_char  *start = u->response_buf->pos;  /* 响应起始位置 */
        size_t   len   = (size_t)(header_end + 4 - start); /* 含 \r\n\r\n */

        ssize_t sent = rps_unix_send(r->connection, start, len);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                r->connection->write->handler = rps_http_proxy_write_continue;
                return RPS_AGAIN;
            }
            return RPS_ERROR;
        }
        if ((size_t)sent < len) {
            /* 部分发送：暂不支持，简化处理 */
            r->connection->write->handler = rps_http_proxy_write_continue;
            return RPS_AGAIN;
        }
    }

    /* 移动 buffer position 穿过头部 */
    u->response_buf->pos = header_end + 4;

    /*
     * 101 发送完毕 → 初始化 WS 上下文并切换为双向转发模式。
     * 此后不再走 HTTP 过滤链，数据裸转。
     */
    {
        rps_ws_ctx_t *ctx;

        ctx = rps_pcalloc(r->pool, sizeof(rps_ws_ctx_t));
        if (ctx == NULL) return RPS_ERROR;

        ctx->to_upstream = rps_buf_create(r->pool, WS_BUF_SIZE);
        ctx->to_client   = rps_buf_create(r->pool, WS_BUF_SIZE);
        if (ctx->to_upstream == NULL || ctx->to_client == NULL)
            return RPS_ERROR;

        u->module_ctx = ctx;

        /* 客户端读 → 转发到后端 */
        r->connection->read->handler    = ws_client_read_handler;
        r->connection->read->data       = r;
        r->connection->read->connection = r->connection;

        /* 后端读 → 转发到客户端 */
        u->peer->read->handler    = ws_upstream_read_handler;
        u->peer->read->data       = r;
        u->peer->read->connection = u->peer;

        /* 删除 HTTP 阶段的读写定时器，WS 有自己的生命周期 */
        rps_event_del_timer(r->connection->read);
        rps_event_del_timer(u->peer->read);

        rps_log_error(RPS_LOG_INFO, r->log, 0,
                      "WS: upgrade complete, bidirectional mode active");
    }

    /*
     * 返回 RPS_AGAIN 阻止 read_handler 进入 READ_BODY 状态机。
     * WS 的后续数据由 ws_upstream_read_handler / ws_client_read_handler 处理。
     */
    return RPS_AGAIN;
}


/*
 * 客户端→后端：读客户端数据，写后端
 */
static void
ws_client_read_handler(rps_event_t *ev)
{
    rps_connection_t   *c = ev->connection;
    rps_http_request_t *r;
    rps_upstream_t     *u;
    rps_ws_ctx_t       *ctx;
    u_char              buf[WS_BUF_SIZE];
    ssize_t             n, sent;

    if (ev->timedout) return;
    if (c == NULL) return;
    r = ev->data;
    if (r == NULL || r->upstream == NULL) {
        ws_close(r);
        return;
    }
    u   = r->upstream;
    ctx = u->module_ctx;
    if (ctx == NULL) return;

    n = recv(c->fd, buf, sizeof(buf), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
    }
    if (n <= 0) {
        /* 客户端关闭或出错 */
        ctx->client_closed = 1;
        if (u->peer && u->peer->fd > 0) {
            shutdown(u->peer->fd, SHUT_WR);
            rps_event_del_timer(u->peer->read);
        }
        if (ev->active)
            c->cycle->event_engine->del_event(ev, RPS_READ_EVENT);
        ws_close(r);
        return;
    }

    /* 写后端 */
    sent = send(u->peer->fd, buf, (size_t)n, MSG_NOSIGNAL);
    if (sent < n) {
        if (sent < 0 && (errno == EAGAIN || errno == EINTR)) sent = 0;

        /* 部分发送：剩余数据存入缓冲，注册写事件 */
        size_t remain = (size_t)n - (sent > 0 ? (size_t)sent : 0);
        if (remain > 0 && ctx->to_upstream) {
            memcpy(ctx->to_upstream->pos, buf + (sent > 0 ? sent : 0), remain);
            ctx->to_upstream->last = ctx->to_upstream->pos + remain;

            /* 暂停客户端读（背压），等后端写就绪 */
            if (ev->active)
                c->cycle->event_engine->del_event(ev, RPS_READ_EVENT);

            u->peer->write->handler = ws_client_write_handler;
            u->peer->write->data    = r; u->peer->write->connection = u->peer;
            if (!u->peer->write->active)
                c->cycle->event_engine->add_event(u->peer->write,
                                                   RPS_WRITE_EVENT);
        }
    }
}

/*
 * 后端→客户端：读后端数据，写客户端
 */
static void
ws_upstream_read_handler(rps_event_t *ev)
{
    rps_connection_t   *c = ev->connection;
    rps_http_request_t *r;
    rps_upstream_t     *u;
    rps_ws_ctx_t       *ctx;

    if (ev->timedout) return;
    u_char              buf[WS_BUF_SIZE];
    ssize_t             n, sent;

    if (c == NULL) return;
    r = ev->data;
    if (r == NULL || r->upstream == NULL) {
        ws_close(r);
        return;
    }
    u   = r->upstream;
    ctx = u->module_ctx;
    if (ctx == NULL) return;

    n = recv(c->fd, buf, sizeof(buf), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
    }
    if (n <= 0) {
        /* 后端关闭或出错 */
        ctx->upstream_closed = 1;
        if (r->connection && r->connection->fd > 0) {
            shutdown(r->connection->fd, SHUT_WR);
            rps_event_del_timer(r->connection->read);
        }
        if (ev->active)
            c->cycle->event_engine->del_event(ev, RPS_READ_EVENT);
        ws_close(r);
        return;
    }

    /* 写客户端 */
    sent = send(r->connection->fd, buf, (size_t)n, MSG_NOSIGNAL);
    if (sent < n) {
        if (sent < 0 && (errno == EAGAIN || errno == EINTR)) sent = 0;

        size_t remain = (size_t)n - (sent > 0 ? (size_t)sent : 0);
        if (remain > 0 && ctx->to_client) {
            memcpy(ctx->to_client->pos, buf + (sent > 0 ? sent : 0), remain);
            ctx->to_client->last = ctx->to_client->pos + remain;

            if (ev->active)
                c->cycle->event_engine->del_event(ev, RPS_READ_EVENT);

            r->connection->write->handler = ws_upstream_write_handler;
            r->connection->write->data    = r; r->connection->write->connection = r->connection;
            if (!r->connection->write->active)
                c->cycle->event_engine->add_event(r->connection->write,
                                                   RPS_WRITE_EVENT);
        }
    }
}

/*
 * 客户端→后端写续传
 */
static void
ws_client_write_handler(rps_event_t *ev)
{
    rps_connection_t   *c = ev->connection;
    rps_http_request_t *r;
    rps_upstream_t     *u;
    rps_ws_ctx_t       *ctx;

    if (ev->timedout) return;
    ssize_t             sent;

    if (c == NULL) return;
    r = ev->data;
    if (r == NULL || r->upstream == NULL) return;
    u   = r->upstream;
    ctx = u->module_ctx;
    if (ctx == NULL || ctx->to_upstream == NULL) return;

    if (ctx->to_upstream->pos >= ctx->to_upstream->last) {
        /* 缓冲已空，恢复客户端读 */
        c->cycle->event_engine->del_event(ev, RPS_WRITE_EVENT);
        r->connection->read->handler = ws_client_read_handler;
        r->connection->read->data    = r; 
        r->connection->read->connection = r->connection;
        if (!r->connection->read->active)
            c->cycle->event_engine->add_event(r->connection->read,
                                               RPS_READ_EVENT);
        return;
    }

    sent = send(c->fd, ctx->to_upstream->pos,
                (size_t)(ctx->to_upstream->last - ctx->to_upstream->pos), 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        ctx->client_closed = 1;
        ws_close(r);
        return;
    }
    ctx->to_upstream->pos += sent;

    if (ctx->to_upstream->pos >= ctx->to_upstream->last) {
        /* 缓冲已空，恢复客户端读 */
        c->cycle->event_engine->del_event(ev, RPS_WRITE_EVENT);
        r->connection->read->handler = ws_client_read_handler;
        r->connection->read->data    = r; 
        r->connection->read->connection = r->connection;
        if (!r->connection->read->active)
            c->cycle->event_engine->add_event(r->connection->read,
                                               RPS_READ_EVENT);
    }
}

/*
 * 后端→客户端写续传
 */
static void
ws_upstream_write_handler(rps_event_t *ev)
{
    rps_connection_t   *c = ev->connection;
    rps_http_request_t *r;
    rps_upstream_t     *u;
    rps_ws_ctx_t       *ctx;

    if (ev->timedout) return;
    ssize_t             sent;

    if (c == NULL) return;
    r = ev->data;
    if (r == NULL || r->upstream == NULL) return;
    u   = r->upstream;
    ctx = u->module_ctx;
    if (ctx == NULL || ctx->to_client == NULL) return;

    if (ctx->to_client->pos >= ctx->to_client->last) {
        c->cycle->event_engine->del_event(ev, RPS_WRITE_EVENT);
        u->peer->read->handler = ws_upstream_read_handler;
        u->peer->read->data    = r; u->peer->read->connection = u->peer;
        if (!u->peer->read->active)
            c->cycle->event_engine->add_event(u->peer->read, RPS_READ_EVENT);
        return;
    }

    sent = send(c->fd, ctx->to_client->pos,
                (size_t)(ctx->to_client->last - ctx->to_client->pos), 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        ctx->upstream_closed = 1;
        ws_close(r);
        return;
    }
    ctx->to_client->pos += sent;

    if (ctx->to_client->pos >= ctx->to_client->last) {
        c->cycle->event_engine->del_event(ev, RPS_WRITE_EVENT);
        u->peer->read->handler = ws_upstream_read_handler;
        u->peer->read->data    = r; u->peer->read->connection = u->peer;
        if (!u->peer->read->active)
            c->cycle->event_engine->add_event(u->peer->read, RPS_READ_EVENT);
    }
}

/*
 * 关闭 WS 连接：清理两侧，最终化 request
 */
static void
ws_close(rps_http_request_t *r)
{
    rps_upstream_t   *u;
    rps_connection_t *c;
    rps_ws_ctx_t     *ctx;

    if (r == NULL) return;

    u = r->upstream;
    c = r->connection;

    if (u) {
        ctx = u->module_ctx;

        /* 两端都已关闭 → 最终清理 */
        if (ctx && ctx->client_closed && ctx->upstream_closed) {
            if (u->peer) {
                if (u->peer->write->active)
                    c->cycle->event_engine->del_event(u->peer->write,
                                                       RPS_WRITE_EVENT);
                if (u->peer->read->active)
                    c->cycle->event_engine->del_event(u->peer->read,
                                                       RPS_READ_EVENT);
                rps_event_del_timer(u->peer->read);
                rps_event_del_timer(u->peer->write);
                rps_upstream_close_peer_conn(u->peer);
                u->peer = NULL;
            }

            r->upstream = NULL;
            if (c) {
                if (c->read->active)
                    c->cycle->event_engine->del_event(c->read, RPS_READ_EVENT);
                if (c->write && c->write->active)
                    c->cycle->event_engine->del_event(c->write, RPS_WRITE_EVENT);
            }

            /* 必须在 finalize（会销毁 r）之前取出 c */
            {
                rps_connection_t *conn = c;
                rps_http_finalize_request(r, RPS_OK);
                if (conn) rps_http_complete_request(conn);
            }
        }
    }
}

#if 0
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
#endif /*old proxy handlers */
