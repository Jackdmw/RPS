#include "http/rps_http_core.h"
#include "http/rps_http_phases.h"
#include "core/rps_connection.h"
#include "core/rps_palloc.h"
#include <sys/socket.h>

/*
 * 发送 HTTP 响应行 + 基本头部
 * 构造 "HTTP/1.1 200 OK\r\nServer: RPS\r\nConnection: keep-alive\r\n\r\n"
 * 然后调用 output_filter 写入 socket
 */
rps_int_t
rps_http_send_header(rps_http_request_t *r)
{
    rps_buf_t  *b;
    rps_chain_t *out;
    u_char     *p;

    b = rps_buf_create(r->pool, 512);
    if (b == NULL) {
        return RPS_ERROR;
    }

    p = b->pos;

    p = rps_cpymem(p, "HTTP/1.1 200 OK\r\n", sizeof("HTTP/1.1 200 OK\r\n") - 1);
    p = rps_cpymem(p, "Server: RPS\r\n",     sizeof("Server: RPS\r\n") - 1);
    p = rps_cpymem(p, "Connection: ",        sizeof("Connection: ") - 1);
    if (r->keepalive) {
        p = rps_cpymem(p, "keep-alive\r\n", sizeof("keep-alive\r\n") - 1);
    } else {
        p = rps_cpymem(p, "close\r\n",      sizeof("close\r\n") - 1);
    }
    p = rps_cpymem(p, "\r\n", 2);

    b->last = p;

    out = rps_palloc(r->pool, sizeof(rps_chain_t));
    if (out == NULL) {
        return RPS_ERROR;
    }
    out->buf  = b;
    out->next = NULL;

    return rps_http_output_filter(r, out);
}

/*
 * 发送响应体：将 body buf 包装成 chain 节点写入 socket
 */
rps_int_t
rps_http_send_body(rps_http_request_t *r, rps_buf_t *body)
{
    rps_chain_t *out;

    out = rps_palloc(r->pool, sizeof(rps_chain_t));
    if (out == NULL) {
        return RPS_ERROR;
    }
    out->buf  = body;
    out->next = NULL;

    return rps_http_output_filter(r, out);
}

/*
 * 输出过滤器：遍历 buffer 链，逐段写入 socket
 *
 * 返回值:
 *   RPS_OK    — 全部写完
 *   RPS_AGAIN — 遇到 EAGAIN，上一段可能部分写入，调用者应在写事件就绪后重试
 *   RPS_ERROR — socket 错误
 */
rps_int_t
rps_http_output_filter(rps_http_request_t *r, rps_chain_t *out)
{
    rps_chain_t *cl;
    ssize_t      n;
    size_t       size;
    for (cl = out; cl != NULL; cl = cl->next) {
        if (cl->buf == NULL) {
            continue;
        }

        size = cl->buf->last - cl->buf->pos;
        if (size == 0) {
            continue;
        }

        n = send(r->connection->fd, cl->buf->pos, size, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                return RPS_AGAIN;
            }
            return RPS_ERROR;
        }
        cl->buf->pos += n;

        if (cl->buf->pos < cl->buf->last) {
            return RPS_AGAIN;
        }
    }
    printf("OK\n");
    return RPS_OK;
}

/*
 * 最终回收请求
 * 标记连接关闭并销毁请求。
 */
void
rps_http_finalize_request(rps_http_request_t *r, rps_int_t rc)
{
    rps_connection_t  *c;

    c = r->connection;

    if (r->keepalive && c && rc == RPS_OK) {
        /* keepalive: 不做清理，由调用者在事件循环中重建 request */
        return;
    }

    if (c) {
        c->close = 1;
    }
    rps_http_close_request(r);
}
