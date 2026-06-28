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


/**
 * 线程池互斥锁初始化，初始化cycle中的可变运行资源，两个空闲连接链表
 */
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
/**
 * 销毁两个互斥锁
 */
void
rps_thread_mutex_destroy(rps_cycle_t *cycle)
{
    pthread_mutex_destroy(&cycle->conn_mutex);
    pthread_mutex_destroy(&cycle->upstream_conn_mutex);
}


/* poll 等待 fd 可读/可写，单位毫秒。返回 1=就绪, 0=超时, -1=错误 */
int
rps_thread_poll_wait(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    int           ret;
    pfd.fd      = fd;
    pfd.events  = events;

    do {
        pfd.revents = 0;
        ret = poll(&pfd, 1, timeout_ms);
    } while (ret == -1 && errno == EINTR);

    return ret;
}

/* 阻塞写，循环直到全部写完或出错，返回 0=成功, -1=失败 */
int
rps_thread_blocking_send_all(int fd, const u_char *buf, size_t len, int timeout_ms)
{
    size_t sent = 0;
    while (sent < len) {
        if (rps_thread_poll_wait(fd, POLLOUT, timeout_ms) <= 0) return -1;
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}


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
        if (rps_thread_poll_wait(r->connection->fd, POLLIN, 60000) <= 0) return -1;
        n = recv(r->connection->fd, b->last, (size_t)(b->end - b->last), 0);
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
    rps_thread_ctx_t          *ctx = arg;
    rps_cycle_t               *cycle = ctx->cycle;
    rps_connection_t          *c = ctx->c;
    rps_http_request_t        *r = ctx->r;
    rps_http_core_main_conf_t *cmcf = NULL;
    rps_http_conf_container_t *container;
    int                        ret;

    /* 获取 main_conf */
    if (ctx->conf_ctx) {
        container = ((void **)ctx->conf_ctx)[rps_http_module.index];
        if (container)
            cmcf = container->main_conf[rps_http_core_module.ctx_index];
    }

    rps_log_error(RPS_LOG_INFO, cycle->log, 0,
                  "[thread %lu] started", (unsigned long)pthread_self());

    /*
     * keepalive 循环：
     *   read_request 失败（ret<=0）→ break，线程自己清理
     *   read_request 成功（ret>0）→ run_phases，内部完成 finalize+complete
     */
    for (;;) {
        ret = thread_read_request(r);
        if (ret <= 0) {
            /* 解析/读失败：请求未被引擎消费，线程自己清理 */
            rps_http_finalize_request(r, RPS_ERROR);
            rps_http_complete_request(c);
            break;
        }

        if (r->main_conf == NULL) {
            container = ((void **)ctx->conf_ctx)[rps_http_module.index];
            if (container) r->main_conf = container->main_conf;
        }
        if (cmcf == NULL)
            cmcf = r->main_conf ? r->main_conf[rps_http_core_module.ctx_index] : NULL;

        if (cmcf == NULL) {
            rps_http_finalize_request(r, RPS_ERROR);
            rps_http_complete_request(c);
            break;
        }

        c->data = NULL;
        rps_http_run_phases(r, cmcf);
        /* 阶段引擎 + upstream_finalize 已处理 request 生命周期 */

        if (c->close) break;
        r = c->data;
        if (r == NULL) break;
    }
    rps_log_error(RPS_LOG_INFO, cycle->log, 0, "[thread %lu] exiting", (unsigned long)pthread_self());
    free(ctx);
    return NULL;
}
