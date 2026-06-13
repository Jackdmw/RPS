#include "rps_thread.h"
#include "http/rps_http_phases.h"
#include "http/rps_http_parse.h"
#include "http/modules/rps_http_core_module.h"
#include "core/rps_log.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

/* ════════════════════════════════════════════════════════════
 * 连接池互斥锁
 * ════════════════════════════════════════════════════════════ */

int
rps_thread_mutex_init(rps_cycle_t *cycle)
{
    if (pthread_mutex_init(&cycle->conn_mutex, NULL) != 0) return RPS_ERROR;
    if (pthread_mutex_init(&cycle->upstream_conn_mutex, NULL) != 0) {
        pthread_mutex_destroy(&cycle->conn_mutex);
        return RPS_ERROR;
    }
    return RPS_OK;
}

void
rps_thread_mutex_destroy(rps_cycle_t *cycle)
{
    pthread_mutex_destroy(&cycle->conn_mutex);
    pthread_mutex_destroy(&cycle->upstream_conn_mutex);
}

/* ════════════════════════════════════════════════════════════
 * 阻塞 I/O 工具
 * ════════════════════════════════════════════════════════════ */

/* poll 等待 fd 可读/可写，单位毫秒。返回 1=就绪, 0=超时, -1=错误 */
static int
poll_wait(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd      = fd;
    pfd.events  = events;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms);
}

/* 阻塞读指定字节数，返回实际读取量，<0=错误 */
static ssize_t
blocking_recv(int fd, u_char *buf, size_t want, int timeout_ms)
{
    if (poll_wait(fd, POLLIN, timeout_ms) <= 0) return -1;
    return recv(fd, buf, want, 0);
}

/* 阻塞写，循环直到全部写完或出错，返回 0=成功, -1=失败 */
static int
blocking_send_all(int fd, const u_char *buf, size_t len, int timeout_ms)
{
    size_t sent = 0;
    while (sent < len) {
        if (poll_wait(fd, POLLOUT, timeout_ms) <= 0) return -1;
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════
 * HTTP 请求阻塞读 + 解析
 * ════════════════════════════════════════════════════════════ */

static int
thread_read_request(rps_http_request_t *r)
{
    rps_buf_t *b = r->request_body;
    ssize_t    n;

    /* 重置解析状态和 buffer */
    r->parse_status = 0;
    b->pos = b->start;
    b->last = b->start;

    for (;;) {
        n = blocking_recv(r->connection->fd, b->last,
                          (size_t)(b->end - b->last), 60000);
        if (n <= 0) return (n == 0) ? 0 : -1;
        b->last += n;

        /* 状态机解析 */
        for (;;) {
            if (r->parse_status == 0) {
                int rc = rps_http_parse_request_line(r);
                if (rc == RPS_HTTP_PARSE_EAGIN) break;
                if (rc == RPS_HTTP_PARSE_ERROR) return -1;
                /* parse_status 0→1 */
            }
            if (r->parse_status == 1) {
                int rc = rps_http_parse_headers(r);
                if (rc == RPS_HTTP_PARSE_EAGIN) break;
                if (rc == RPS_HTTP_PARSE_ERROR) return -1;
                /* parse_status 1→2 */
            }
            if (r->parse_status == 2) {
                /* Content-Length body */
                if (r->headers_in.content_length_n > 0) {
                    size_t hdr_len = (size_t)(b->pos - b->start);
                    size_t total   = hdr_len + r->headers_in.content_length_n;
                    size_t have    = (size_t)(b->last - b->start);
                    if (have < total) break;
                }
                return 1;
            }
        }
    }
}

/* 重置 request 用于 keepalive 下一个请求 */
static void
thread_reset_request(rps_http_request_t *r)
{
    rps_memzero(&r->headers_in, sizeof(rps_http_headers_in_t));
    rps_memzero(&r->headers_out, sizeof(rps_http_headers_out_t));

    r->headers_in.connection.key     = (rps_str_t)rps_string("connection");
    r->headers_in.content_length.key = (rps_str_t)rps_string("content-length");
    r->headers_in.user_agent.key     = (rps_str_t)rps_string("user-agent");
    r->headers_in.content_type.key   = (rps_str_t)rps_string("content-type");
    r->headers_in.upgrade.key        = (rps_str_t)rps_string("upgrade");

    r->headers_out.content_type.key = (rps_str_t)rps_string("content-type");
    r->headers_out.server.key       = (rps_str_t)rps_string("server");
    r->headers_out.status.key       = (rps_str_t)rps_string("status");

    r->headers_out.status.value           = (rps_str_t)rps_string("200 OK");
    r->headers_out.server.value           = (rps_str_t)rps_string("RPS");
    r->headers_out.content_type.value     = (rps_str_t)rps_null_string;
    r->headers_out.content_length_n.value = (rps_str_t)rps_null_string;

    r->headers_in.headers_n  = 0;
    r->headers_out.headers_n = 0;

    r->method       = (rps_str_t)rps_null_string;
    r->uri          = (rps_str_t)rps_null_string;
    r->args         = (rps_str_t)rps_null_string;
    r->http_version = (rps_str_t)rps_null_string;
    r->host         = (rps_str_t)rps_null_string;

    r->out_chain    = NULL;
    r->upstream     = NULL;
    r->loc_conf     = NULL;
    r->srv_conf     = NULL;
    r->uri_changed  = 0;
    r->internal_redirect = 0;
    r->phase_index  = 0;

    r->request_body->pos   = r->request_body->start;
    r->request_body->last  = r->request_body->start;
    r->request_body_rest   = 0;
    r->reading_body        = 0;

    r->keepalive = 1;
}

/* ════════════════════════════════════════════════════════════
 * 线程入口
 * ════════════════════════════════════════════════════════════ */

int
rps_thread_spawn(rps_cycle_t *cycle, rps_connection_t *c,
                  rps_http_request_t *r)
{
    rps_thread_ctx_t *ctx;
    pthread_t         tid;
    pthread_attr_t    attr;

    ctx = malloc(sizeof(rps_thread_ctx_t));
    if (ctx == NULL) return RPS_ERROR;

    ctx->cycle    = cycle;
    ctx->c        = c;
    ctx->r        = r;
    ctx->conf_ctx = cycle->conf_ctx;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&tid, &attr, rps_thread_worker, ctx) != 0) {
        pthread_attr_destroy(&attr);
        free(ctx);
        return RPS_ERROR;
    }

    pthread_attr_destroy(&attr);
    return RPS_OK;
}

void *
rps_thread_worker(void *arg)
{
    rps_thread_ctx_t         *ctx = arg;
    rps_cycle_t              *cycle = ctx->cycle;
    rps_connection_t         *c = ctx->c;
    rps_http_request_t       *r = ctx->r;
    rps_http_core_main_conf_t *cmcf = NULL;
    rps_http_conf_container_t *container;
    int                       ret;

    /* 获取 main_conf */
    if (ctx->conf_ctx) {
        container = ((void **)ctx->conf_ctx)[rps_http_module.index];
        if (container)
            cmcf = container->main_conf[rps_http_core_module.ctx_index];
    }

    rps_log_error(RPS_LOG_INFO, cycle->log, 0,
                  "[thread %lu] started", (unsigned long)pthread_self());

    /* keepalive 循环 */
    for (;;) {
        int request_finalized = 0;

        ret = thread_read_request(r);
        if (ret <= 0) {
            if (ret == 0)
                rps_log_error(RPS_LOG_DEBUG, cycle->log, 0,
                              "[thread] client closed");
            else
                rps_log_error(RPS_LOG_ERR, cycle->log, errno,
                              "[thread] read error");
            break;
        }

        if (r->main_conf == NULL) {
            container = ((void **)ctx->conf_ctx)[rps_http_module.index];
            if (container) r->main_conf = container->main_conf;
        }
        if (cmcf == NULL) cmcf = r->main_conf
            ? r->main_conf[rps_http_core_module.ctx_index] : NULL;

        if (cmcf == NULL) {
            rps_log_error(RPS_LOG_ERR, cycle->log, 0,
                          "[thread] no config");
            break;
        }

        /* 同步执行阶段引擎 */
        c->data = NULL;
        rps_http_run_phases(r, cmcf);

        /*
         * 阶段引擎可能已内部 finalize r（如 proxy 非线程模式返回 RPS_OK、
         * 默认 handler 等路径）。通过 c->data 是否仍指向 r 来判断：
         *   c->data == r  → r 仍存活，线程负责 finalize
         *   c->data != r  → r 已被 finalize + complete_request 销毁
         */
        if (c->data != r) {
            request_finalized = 1;
            /* complete_request 已创建新 request 或释放了连接 */
            if (c->data != NULL && c->close == 0) {
                /* keepalive：complete_request 创建了新 request */
                r = c->data;
                r->start_msec = rps_current_msec();
                continue;
            }
            break;
        }

        if (!r->keepalive) {
            rps_log_error(RPS_LOG_DEBUG, cycle->log, 0,
                          "[thread] keepalive off, closing");
            break;
        }

        thread_reset_request(r);
    }

    /*
     * 如果请求未被阶段引擎 finalize，由线程自己清理。
     * c->data == r 表示 r 仍存活。
     */
    if (c->data == r) {
        rps_http_finalize_request(r, RPS_OK);
    }

    /* 归还连接（线程安全） */
    pthread_mutex_lock(&cycle->conn_mutex);
    if (c->close == 0) rps_free_connection(c);
    pthread_mutex_unlock(&cycle->conn_mutex);

    rps_log_error(RPS_LOG_INFO, cycle->log, 0,
                  "[thread %lu] exiting", (unsigned long)pthread_self());
    free(ctx);
    return NULL;
}
