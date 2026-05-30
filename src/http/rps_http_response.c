#include "http/rps_http_core.h"
#include "http/rps_http_phases.h"
#include "core/rps_connection.h"
#include "core/rps_palloc.h"
#include <sys/socket.h>

/*
 * 往 headers_out 追加一个 header
 */
rps_int_t
rps_http_add_response_header(rps_http_request_t *r, rps_str_t key, rps_str_t value)
{
    rps_http_header_kv_t *h;

    h = rps_list_push(&r->headers_out.headers);
    if (h == NULL) return RPS_ERROR;

    h->key   = key;
    h->value = value;
    return RPS_OK;
}

/*
 * 发送 HTTP 响应：从 r->headers_out 序列化状态行和所有 header，写入 socket
 */
rps_int_t
rps_http_send_header(rps_http_request_t *r)
{
    rps_buf_t              *b;
    rps_chain_t            *out;
    u_char                 *p;
    rps_http_header_kv_t   *hdr;
    rps_list_part_t        *part;
    rps_uint_t              i;
    rps_str_t               status_line;

    b = rps_buf_create(r->pool, 512);
    if (b == NULL) {
        return RPS_ERROR;
    }

    p = b->pos;

    /* ── 状态行 ── */
    if (r->headers_out.status.value.data != NULL) {
        status_line = r->headers_out.status.value;
    } else {
        status_line = (rps_str_t)rps_string("200 OK");
    }

    if (r->http_version.data != NULL) {
        p = rps_cpymem(p, r->http_version.data, r->http_version.len);
    } else {
        p = rps_cpymem(p, "HTTP/1.1", 8);
    }
    *p++ = ' ';
    p = rps_cpymem(p, status_line.data, status_line.len);
    *p++ = '\r'; *p++ = '\n';

    /* ── Server ── */
    if (r->headers_out.server.value.data != NULL) {
        p = rps_cpymem(p, "Server: ", 8);
        p = rps_cpymem(p, r->headers_out.server.value.data,
                       r->headers_out.server.value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* ── Content-Type ── */
    if (r->headers_out.content_type.value.data != NULL) {
        p = rps_cpymem(p, "Content-Type: ", 14);
        p = rps_cpymem(p, r->headers_out.content_type.value.data,
                       r->headers_out.content_type.value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* ── Content-Length ── */
    if (r->headers_out.content_length_n.value.data != NULL) {
        p = rps_cpymem(p, "Content-Length: ", 16);
        p = rps_cpymem(p, r->headers_out.content_length_n.value.data,
                       r->headers_out.content_length_n.value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* ── Connection ── */
    p = rps_cpymem(p, "Connection: ", 12);
    if (r->keepalive) {
        p = rps_cpymem(p, "keep-alive\r\n", 12);
    } else {
        p = rps_cpymem(p, "close\r\n", 7);
    }

    /* ── 自定义 header ── */
    part = &r->headers_out.headers.part;
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

    /* ── 空行（header 结束标记）─── */
    *p++ = '\r'; *p++ = '\n';

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
    return RPS_OK;
}

/*
 * 最终回收请求
 * 这里只负责清理请求资源。
 * 对于keepalive的请求，会给conn打上close标记
 */
void
rps_http_finalize_request(rps_http_request_t *r, rps_int_t rc)
{
    rps_connection_t  *c;
    unsigned           keepalive;

    c        = r->connection;
    keepalive = r->keepalive;

    if (rc != RPS_OK) {
        keepalive = 0;
    }

    rps_http_close_request(r);

    if (c) {
        c->close = !keepalive;
    }
}
