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

static int poll_wait(int fd, short events, int timeout_ms);
static int blocking_send_all(int fd, const u_char *buf, size_t len, int timeout_ms);

/* ════════════════════════════════════════════════════════════
 * 线程启动时注册给 proxy handler 的入口
 * ════════════════════════════════════════════════════════════ */

rps_int_t
rps_thread_proxy_run(rps_http_request_t *r, rps_upstream_t *u)
{
    int             fd;
    struct addrinfo hints, *res, *rp;
    char            host_buf[256], port_buf[8];
    size_t          host_len;
    rps_int_t       rc;
    ssize_t         n;

    /* ── 1. 构造请求 ── */
    if (u->create_request) {
        if (u->create_request(r, u) != RPS_OK) return RPS_ERROR;
    }

    /* ── 2. 阻塞 connect ── */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return RPS_ERROR;

    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    host_len = u->peer_addr.host.len;
    if (host_len >= sizeof(host_buf)) host_len = sizeof(host_buf) - 1;
    memcpy(host_buf, u->peer_addr.host.data, host_len);
    host_buf[host_len] = '\0';
    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)u->peer_addr.port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host_buf, port_buf, &hints, &res) != 0) {
        close(fd); return RPS_ERROR;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        if (errno == EINPROGRESS) {
            /* 等连接完成 */
            if (poll_wait(fd, POLLOUT, (int)u->connect_timeout) <= 0) continue;
            {
                int       sock_err = 0;
                socklen_t len      = sizeof(sock_err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) == 0
                    && sock_err == 0) break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (rp == NULL) return RPS_ERROR;

    /* ── 3. 阻塞发请求 ── */
    {
        size_t len = (size_t)(u->request_bufs->last - u->request_bufs->pos);
        if (blocking_send_all(fd, u->request_bufs->pos, len,
                              (int)u->send_timeout) != 0) {
            close(fd); return RPS_ERROR;
        }
    }

    /* ── 4. 阻塞收响应头 ── */
    {
        u_char buf[16384];
        size_t buf_used = 0;

        u->response_buf->pos  = u->response_buf->start;
        u->response_buf->last = u->response_buf->start;

        while (!u->header_complete) {
            if (poll_wait(fd, POLLIN, POLL_TIMEOUT) <= 0) { close(fd); return RPS_ERROR; }
            n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) { close(fd); return RPS_ERROR; }

            /* 追加到 response_buf */
            if ((size_t)n > (size_t)(u->response_buf->end - u->response_buf->last)) {
                close(fd); return RPS_ERROR;  /* 缓冲不够 */
            }
            memcpy(u->response_buf->last, buf, (size_t)n);
            u->response_buf->last += n;

            if (u->process_header) {
                rc = u->process_header(r, u);
                if (rc == RPS_AGAIN) continue;  /* 头不完整 */
                if (rc != RPS_OK) { close(fd); return RPS_ERROR; }
                break;
            }
        }
    }

    /* ── 5. 收 body + 转发客户端 ── */
    while (u->content_length_n == 0
           || u->body_received < u->content_length_n)
    {
        u_char buf[16384];

        if (poll_wait(fd, POLLIN, POLL_TIMEOUT) <= 0) { close(fd); return RPS_ERROR; }
        n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;  /* EOF */
        if (n < 0) { close(fd); return RPS_ERROR; }

        u->body_received += (size_t)n;

        /* 追加到 response_buf */
        if ((size_t)(u->response_buf->end - u->response_buf->last) < (size_t)n) {
            close(fd); return RPS_ERROR;
        }
        memcpy(u->response_buf->last, buf, (size_t)n);
        u->response_buf->last += n;

        /* 通过 HTTP 过滤链转发给客户端 */
        if (u->forward_body) {
            rc = u->forward_body(r, u);
            if (rc == RPS_AGAIN) {
                /* write_filter 可能返回 AGAIN（客户端写阻塞）
                   在线程模式中改用 poll 阻塞等 */
                rc = rps_http_write_filter(r);
                /* 此时应该全部写完 */
            }
            if (rc != RPS_OK) { close(fd); return RPS_ERROR; }
        }
    }

    close(fd);
    return RPS_HTTP_DONE;
}

/* ════════════════════════════════════════════════════════════
 * WebSocket: 完整流程（connect → 握手 → poll 双向转发）
 * ════════════════════════════════════════════════════════════ */

rps_int_t
rps_thread_ws_start(rps_http_request_t *r, rps_upstream_t *u)
{
    rps_int_t rc;

    /* 复用 thread_proxy_run 做握手阶段的 connect + send + recv headers */
    rc = rps_thread_proxy_run(r, u);
    if (rc != RPS_HTTP_DONE) return RPS_ERROR;

    /*
     * ws_process_header 已将 101 发给客户端、初始化 ws_ctx。
     * 但后端 fd 在 thread_proxy_run 中已被 close(fd)——不能复用。
     * 需要在这里保留 fd 然后进入转发循环。
     *
     * 简单方案：重新连接并握手，或修改 proxy_run 返回保持 fd。
     * 当前采用更直接的方式：在 ws_start 中完成全部操作。
     */

    /* 重新实现 WS 握手 + 转发，保持 fd 存活 */
    {
        int fd;
        struct addrinfo hints, *res, *rp;
        char    host_buf[256], port_buf[8];
        size_t  host_len;
        ssize_t n;

        /* connect */
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return RPS_ERROR;
        {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        host_len = u->peer_addr.host.len;
        if (host_len >= sizeof(host_buf)) host_len = sizeof(host_buf) - 1;
        memcpy(host_buf, u->peer_addr.host.data, host_len);
        host_buf[host_len] = '\0';
        snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)u->peer_addr.port);
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host_buf, port_buf, &hints, &res) != 0) { close(fd); return RPS_ERROR; }
        for (rp = res; rp; rp = rp->ai_next) {
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
            if (errno == EINPROGRESS) {
                if (poll_wait(fd, POLLOUT, (int)u->connect_timeout) <= 0) continue;
                int e; socklen_t l = sizeof(e);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) == 0 && e == 0) break;
            }
            close(fd); fd = -1;
        }
        freeaddrinfo(res);
        if (rp == NULL) return RPS_ERROR;

        /* send upgrade request */
        if (u->create_request) u->create_request(r, u);
        {
            size_t len = (size_t)(u->request_bufs->last - u->request_bufs->pos);
            if (blocking_send_all(fd, u->request_bufs->pos, len, POLL_TIMEOUT) != 0) {
                close(fd); return RPS_ERROR;
            }
        }

        /* recv 101 */
        u->response_buf->pos  = u->response_buf->start;
        u->response_buf->last = u->response_buf->start;
        while (!u->header_complete) {
            u_char buf[16384];
            if (poll_wait(fd, POLLIN, POLL_TIMEOUT) <= 0) { close(fd); return RPS_ERROR; }
            n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) { close(fd); return RPS_ERROR; }
            memcpy(u->response_buf->last, buf, (size_t)n);
            u->response_buf->last += n;
            if (u->process_header) {
                rc = u->process_header(r, u);
                if (rc == RPS_AGAIN) continue;
                if (rc != RPS_OK) { close(fd); return RPS_ERROR; }
                break;
            }
        }

        /* 把 peer 设置为已连接的 fd */
        {
            rps_connection_t *peer;
            peer = rps_pcalloc(r->pool, sizeof(rps_connection_t));
            if (peer == NULL) { close(fd); return RPS_ERROR; }
            peer->fd    = fd;
            peer->cycle = r->cycle;
            u->peer     = peer;
        }

        /* 进入 poll 双向转发 */
        rps_thread_ws_forward(r, u);
    }

    return RPS_HTTP_DONE;
}

/* ════════════════════════════════════════════════════════════
 * WebSocket poll 双向转发
 * ════════════════════════════════════════════════════════════ */

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
            if (blocking_send_all(backend_fd, buf, (size_t)n, POLL_TIMEOUT) != 0)
                break;
        }

        /* 后端 → 客户端 */
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            n = recv(backend_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (blocking_send_all(client_fd, buf, (size_t)n, POLL_TIMEOUT) != 0)
                break;
        }
    }

    rps_log_error(RPS_LOG_INFO, r->log, 0, "[thread] WS forward ended");

    /* 关闭两侧 */
    close(backend_fd);
    close(client_fd);
}

/* ════════════════════════════════════════════════════════════
 * I/O 工具
 * ════════════════════════════════════════════════════════════ */

static int
poll_wait(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd      = fd;
    pfd.events  = events;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms);
}

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
