#define _GNU_SOURCE
#include "rps_thread.h"
#include "http/modules/rps_http_proxy_module.h"
#include "http/rps_http_phases.h"
#include "core/rps_log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>

#define POLL_TIMEOUT 60000   /* 默认 poll 超时 60s */

/*
 * 阻塞版 write_filter：遍历 out_chain，逐 buf 阻塞 send。
 * 线程模式专用——不依赖 epoll 事件，send 阻塞时 poll-wait。
 */
static int
rps_thread_write_chain(rps_http_request_t *r, int timeout_ms)
{
    rps_chain_t *cl;

    for (cl = r->out_chain; cl != NULL; cl = cl->next) {
        if (cl->buf == NULL) continue;

        while (cl->buf->pos < cl->buf->last) {
            size_t size = (size_t)(cl->buf->last - cl->buf->pos);
            ssize_t n = send(r->connection->fd, cl->buf->pos, size, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    if (rps_thread_poll_wait(r->connection->fd, POLLOUT,
                                             timeout_ms) <= 0)
                        return RPS_ERROR;
                    continue;
                }
                return RPS_ERROR;
            }
            cl->buf->pos += n;
        }
    }
    return RPS_OK;
}



rps_int_t
rps_thread_proxy_run(rps_http_request_t *r, rps_upstream_t *u)
{
    int       fd;
    rps_int_t rc;
    ssize_t   n;
    u_char    buf[16384];
    /* ── 1. prepare：选 peer + 构造请求 + 获取连接（复用 upstream 逻辑）── */
    if (rps_upstream_prepare(r, u) != RPS_OK) return RPS_ERROR;

    fd = u->peer->fd;
    /* ── 2. poll-wait connect（线程 IO）── */
    if (rps_thread_poll_wait(fd, POLLOUT, (int)u->connect_timeout) <= 0){ 
        
        rps_upstream_finalize(r, RPS_ERROR);
        return RPS_ERROR; 
    }
    {
        int       sock_err = 0;
        socklen_t len      = sizeof(sock_err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) < 0
            || sock_err != 0){ 
                
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR; 
            }
    }

    /* ── 3. 阻塞发请求（线程 IO）── */
    {
        size_t len = (size_t)(u->request_bufs->last - u->request_bufs->pos);
        if (rps_thread_blocking_send_all(fd, u->request_bufs->pos, len,(int)u->send_timeout) != 0){ 
            
            rps_upstream_finalize(r, RPS_ERROR);
            return RPS_ERROR; 
        }
    }

    /* ── 4. 阻塞收响应头（复用 process_response 回调）── */
    {
        u->response_buf->pos  = u->response_buf->start;
        u->response_buf->last = u->response_buf->start;
        u->read_state         = RPS_UPSTREAM_READ_HEADER;

        while (!u->header_complete) {
            if (rps_thread_poll_wait(fd, POLLIN, POLL_TIMEOUT) <= 0){ 
                
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR; 
            }
            n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) { 
                
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR; 
            }

            if ((size_t)n > (size_t)(u->response_buf->end - u->response_buf->last)){ 
                
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR; 
            }
            memcpy(u->response_buf->last, buf, (size_t)n);
            u->response_buf->last += n;

            rc = rps_http_proxy_parse_response(r, u);
            if (rc == RPS_AGAIN) continue;
            if (rc != RPS_OK) { 
                
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR; 
            }
        }
        /* 解析完成：发送 headers + 挂 body chain */
        u->headers_sent = 1;
        u->read_state   = RPS_UPSTREAM_READ_BODY;
        if (rps_http_header_filter(r) != RPS_OK) {
            
            rps_upstream_finalize(r, RPS_ERROR);
            return RPS_ERROR;
        }
        {
            rps_chain_t *cl = rps_palloc(r->pool, sizeof(rps_chain_t));
            rps_chain_t *last;
            if (cl == NULL) {
                
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR;
            }
            cl->buf  = u->response_buf;
            cl->next = NULL;
            if (r->out_chain == NULL) r->out_chain = cl;
            else {
                for (last = r->out_chain; last->next; last = last->next);
                last->next = cl;
            }
        }
        /* 拼完 chain 立刻发送一次（header + 已有 body） */
        if (rps_thread_write_chain(r, POLL_TIMEOUT) != RPS_OK){ 
            
            rps_upstream_finalize(r, RPS_ERROR);
            return RPS_ERROR; 
        }

        /* 无 body 的响应（204/304）：已发完，直接完成 */
        if (u->content_length_n == 0) {
            rps_upstream_finalize(r, RPS_OK);
            return RPS_HTTP_DONE;
        }
    }

    /* ── 5. 收 body + 转发客户端 ── */
    {
        while (u->body_received < u->content_length_n) {
            if (rps_thread_poll_wait(fd, POLLIN, POLL_TIMEOUT) <= 0){
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR;
            }
            n = recv(fd, buf, sizeof(buf), 0);
            if (n == 0) break;
            if (n < 0) {
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR;
            }

            u->body_received += (size_t)n;

            if ((size_t)(u->response_buf->end - u->response_buf->last) < (size_t)n){
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR;
            }
            memcpy(u->response_buf->last, buf, (size_t)n);
            u->response_buf->last += n;

            if (rps_thread_write_chain(r, POLL_TIMEOUT) != RPS_OK) {
                rps_upstream_finalize(r, RPS_ERROR);
                return RPS_ERROR;
            }
            if (u->response_buf->pos == u->response_buf->last) {
                u->response_buf->pos  = u->response_buf->start;
                u->response_buf->last = u->response_buf->start;
            }
        }
    }

    rps_upstream_finalize(r, RPS_OK);
    return RPS_HTTP_DONE;
}


rps_int_t
rps_thread_ws_start(rps_http_request_t *r, rps_upstream_t *u)
{
    int       fd;
    ssize_t   n;
    rps_int_t rc;

    /* ── 1. prepare：选 peer + 构造请求 + 获取连接（复用 upstream）── */
    if (rps_upstream_prepare(r, u) != RPS_OK) return RPS_ERROR;
    fd = u->peer->fd;

    /* ── 2. poll-wait connect + 阻塞发请求 ── */
    if (rps_thread_poll_wait(fd, POLLOUT, (int)u->connect_timeout) <= 0){ 
        rps_upstream_finalize(r, RPS_ERROR); 
        return RPS_ERROR; 
    }
    {
        int e; socklen_t l = sizeof(e);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) < 0 || e != 0){ 
            rps_upstream_finalize(r, RPS_ERROR); 
            return RPS_ERROR; 
        }
    }
    {
        size_t len = (size_t)(u->request_bufs->last - u->request_bufs->pos);
        if (rps_thread_blocking_send_all(fd, u->request_bufs->pos, len, POLL_TIMEOUT) != 0){ 
            rps_upstream_finalize(r, RPS_ERROR); 
            return RPS_ERROR; 
        }
    }

    /* ── 3. 收 101 + 校验 + 透传 ── */
    u->response_buf->pos  = u->response_buf->start;
    u->response_buf->last = u->response_buf->start;
    {
        u_char buf[16384];
        while (1) {
            if (rps_thread_poll_wait(fd, POLLIN, POLL_TIMEOUT) <= 0){ 
                rps_upstream_finalize(r, RPS_ERROR); 
                return RPS_ERROR; 
            }
            n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0){ 
                rps_upstream_finalize(r, RPS_ERROR); 
                return RPS_ERROR; 
            }
            if ((size_t)n > (size_t)(u->response_buf->end - u->response_buf->last)){ 
                rps_upstream_finalize(r, RPS_ERROR); 
                return RPS_ERROR; 
            }
            memcpy(u->response_buf->last, buf, (size_t)n);
            u->response_buf->last += n;

            rc = ws_check_101(u, NULL);
            if (rc == RPS_AGAIN) continue;
            if (rc != RPS_OK){ 
                rps_upstream_finalize(r, RPS_ERROR); 
                return RPS_ERROR; 
            }
            break;
        }
    }

    /* 透传 101 给客户端 */
    {
        size_t len = (size_t)(u->response_buf->last - u->response_buf->pos);
        if (rps_thread_blocking_send_all(r->connection->fd,
                               u->response_buf->pos, len, POLL_TIMEOUT) != 0)
            { rps_upstream_finalize(r, RPS_ERROR); return RPS_ERROR; }
    }

    /* ── 4. poll 双向转发 ── */
    rps_thread_ws_forward(r, u);
    return RPS_HTTP_DONE;
}

void
rps_thread_ws_forward(rps_http_request_t *r, rps_upstream_t *u)
{
    int             client_fd  = r->connection->fd;
    int             backend_fd = u->peer ? u->peer->fd : -1;
    struct pollfd   pfds[2];
    u_char          buf[65536];
    ssize_t         n;
    nfds_t          nfds;

    if (backend_fd < 0) return;

    pfds[0].fd     = client_fd;
    pfds[1].fd     = backend_fd;

    rps_log_error(RPS_LOG_INFO, r->log, 0,
                  "[thread] WS forward: client_fd=%d backend_fd=%d",
                  client_fd, backend_fd);

    while (1) {
        pfds[0].events = pfds[1].events = POLLIN;
        pfds[0].revents = pfds[1].revents = 0;
        nfds = 2;

        if (poll(pfds, nfds, 3600000) <= 0) break;  /* 1h 超时 */

        /* 客户端 → 后端 */
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (rps_thread_blocking_send_all(backend_fd, buf, (size_t)n, POLL_TIMEOUT) != 0)
                break;
        }

        /* 后端 → 客户端 */
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            n = recv(backend_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (rps_thread_blocking_send_all(client_fd, buf, (size_t)n, POLL_TIMEOUT) != 0)
                break;
        }
    }

    rps_log_error(RPS_LOG_INFO, r->log, 0, "[thread] WS forward ended");
    rps_upstream_finalize(r, RPS_OK);
}

/* I/O 工具：rps_thread_poll_wait / rps_thread_blocking_send_all 在 rps_thread.c */
