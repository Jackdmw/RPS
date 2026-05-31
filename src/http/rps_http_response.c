#include "http/rps_http_core.h"
#include "http/rps_http_phases.h"
#include "core/rps_connection.h"
#include "core/rps_palloc.h"
#include <sys/socket.h>

/* forward declarations */
static rps_int_t rps_http_header_filter(rps_http_request_t *r);
static rps_int_t rps_http_write_filter(rps_http_request_t *r);
static void rps_http_write_filter_continue(rps_event_t *ev);

/*
 * 往 headers_out 设置 Content-Length
 */
void
rps_http_set_content_length(rps_http_request_t *r, size_t len)
{
    char buf[32];
    int  n;

    n = snprintf(buf, sizeof(buf), "%zu", len);
    r->headers_out.content_length_n.value.data = rps_palloc(r->pool, (size_t)n + 1);
    if (r->headers_out.content_length_n.value.data != NULL) {
        memcpy(r->headers_out.content_length_n.value.data, buf, (size_t)n + 1);
        r->headers_out.content_length_n.value.len = (rps_uint_t)n;
    }
}

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

/* ────────────────────────────────────────────────────────────
 * header_filter:  自动补齐头部 → 序列化 → 插入 out_chain 头部
 * ──────────────────────────────────────────────────────────── */
static rps_int_t
rps_http_header_filter(rps_http_request_t *r)
{
    rps_buf_t              *b;
    rps_chain_t            *hchain;
    u_char                 *p;
    rps_http_header_kv_t   *hdr;
    rps_list_part_t        *part;
    rps_uint_t              i;
    rps_str_t               status_line;

    /* ── 自动补齐 Content-Type ── */
    if (r->headers_out.content_type.value.data == NULL
        || r->headers_out.content_type.value.len == 0)
    {
        r->headers_out.content_type.value = (rps_str_t)rps_string("text/html");
    }

    /* ── 自动补齐 Content-Length ── */
    if (r->headers_out.content_length_n.value.data == NULL
        && r->out_chain != NULL)
    {
        size_t      total = 0;
        rps_chain_t *cl;

        for (cl = r->out_chain; cl != NULL; cl = cl->next) {
            if (cl->buf != NULL) {
                total += (size_t)(cl->buf->last - cl->buf->pos);
            }
        }
        rps_http_set_content_length(r, total);
    }

    /* ── 序列化头部 ── */
    b = rps_buf_create(r->pool, 1024);
    if (b == NULL) {
        return RPS_ERROR;
    }

    p = b->pos;

    /* 状态行 */
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

    /* Server */
    if (r->headers_out.server.value.data != NULL) {
        p = rps_cpymem(p, "Server: ", 8);
        p = rps_cpymem(p, r->headers_out.server.value.data,
                       r->headers_out.server.value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* Content-Type */
    if (r->headers_out.content_type.value.data != NULL) {
        p = rps_cpymem(p, "Content-Type: ", 14);
        p = rps_cpymem(p, r->headers_out.content_type.value.data,
                       r->headers_out.content_type.value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* Content-Length */
    if (r->headers_out.content_length_n.value.data != NULL) {
        p = rps_cpymem(p, "Content-Length: ", 16);
        p = rps_cpymem(p, r->headers_out.content_length_n.value.data,
                       r->headers_out.content_length_n.value.len);
        *p++ = '\r'; *p++ = '\n';
    }

    /* Connection / Keep-Alive */
    p = rps_cpymem(p, "Connection: ", 12);
    if (r->keepalive) {
        p = rps_cpymem(p, "keep-alive\r\n", 12);
        p = rps_cpymem(p, "Keep-Alive: timeout=60\r\n", 24);
    } else {
        p = rps_cpymem(p, "close\r\n", 7);
    }

    /* 自定义 header */
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

    /* 空行（header 结束标记） */
    *p++ = '\r'; *p++ = '\n';

    b->last = p;

    /* 将 header buffer 插入 out_chain 头部 */
    hchain = rps_palloc(r->pool, sizeof(rps_chain_t));
    if (hchain == NULL) {
        return RPS_ERROR;
    }
    hchain->buf  = b;
    hchain->next = r->out_chain;
    r->out_chain = hchain;

    return RPS_OK;
}

/* ────────────────────────────────────────────────────────────
 * write_filter:  遍历 out_chain，逐段 send() 到客户端
 *                EAGAIN → 注册 c->write 事件，下次续传
 * ──────────────────────────────────────────────────────────── */
static rps_int_t
rps_http_write_filter(rps_http_request_t *r)
{
    rps_connection_t *c;
    rps_chain_t      *cl;
    ssize_t           n;
    size_t            size;

    c = r->connection;

    for (cl = r->out_chain; cl != NULL; cl = cl->next) {
        if (cl->buf == NULL) {
            continue;
        }

        size = (size_t)(cl->buf->last - cl->buf->pos);
        if (size == 0) {
            continue;
        }

        n = send(c->fd, cl->buf->pos, size, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                goto eagain;
            }
            return RPS_ERROR;
        }
        cl->buf->pos += n;

        if (cl->buf->pos < cl->buf->last) {
            /* 部分写入，需要等 socket 再次可写 */
            goto eagain;
        }
    }

    /* 全部写完 */
    return RPS_OK;

eagain:
    /* 注册写事件，等待 socket 可写时继续发送 */
    if (!c->write->active) {
        c->write->handler = rps_http_write_filter_continue;
        c->write->data    = r;

        if (c->cycle->event_engine->add_event(c->write,
                                              RPS_WRITE_EVENT) != RPS_OK) {
            return RPS_ERROR;
        }
        rps_event_add_timer(c->write, 60000);
    }
    return RPS_AGAIN;
}

/* ────────────────────────────────────────────────────────────
 * write_filter_continue:  c->write 事件就绪后的续传 handler
 * ──────────────────────────────────────────────────────────── */
static void
rps_http_write_filter_continue(rps_event_t *ev)
{
    rps_http_request_t *r;
    rps_connection_t   *c;
    rps_cycle_t        *cycle;
    rps_int_t           rc;

    r = ev->data;
    if (r == NULL) {
        return;
    }

    c     = r->connection;
    cycle = c->cycle;

    /* 写超时 */
    if (ev->timedout) {
        cycle->event_engine->del_event(c->write, RPS_WRITE_EVENT);
        c->write->active = 0;
        rps_event_del_timer(ev);
        rps_http_finalize_request(r, RPS_ERROR);
        rps_http_complete_request(c);
        return;
    }

    rc = rps_http_write_filter(r);

    if (rc == RPS_OK) {
        /* 全部数据发送完毕 */
        cycle->event_engine->del_event(c->write, RPS_WRITE_EVENT);
        c->write->active = 0;
        rps_event_del_timer(ev);
        rps_http_finalize_request(r, RPS_OK);
        rps_http_complete_request(c);
    } else if (rc == RPS_ERROR) {
        /* 发送失败 */
        cycle->event_engine->del_event(c->write, RPS_WRITE_EVENT);
        c->write->active = 0;
        rps_event_del_timer(ev);
        rps_http_finalize_request(r, RPS_ERROR);
        rps_http_complete_request(c);
    }
    /* RPS_AGAIN: 事件保持注册，等待下次可写 */
}

/* ────────────────────────────────────────────────────────────
 * rps_http_send_response:  统一的响应发送入口
 *                          handler 只需设置 headers_out 和
 *                          out_chain，然后调用此函数即可
 * ──────────────────────────────────────────────────────────── */
rps_int_t
rps_http_send_response(rps_http_request_t *r)
{
    if (rps_http_header_filter(r) != RPS_OK) {
        return RPS_ERROR;
    }
    /* body_filter 跳过（后续可插入 chunked / gzip / range） */
    return rps_http_write_filter(r);
}

/* ────────────────────────────────────────────────────────────
 * rps_http_send_header:  向后兼容：单独发送响应头（无 body）
 *                        内部使用 header_filter + write_filter
 * ──────────────────────────────────────────────────────────── */
rps_int_t
rps_http_send_header(rps_http_request_t *r)
{
    if (rps_http_header_filter(r) != RPS_OK) {
        return RPS_ERROR;
    }
    return rps_http_write_filter(r);
}

/* ────────────────────────────────────────────────────────────
 * rps_http_send_body:  向后兼容：追加 body 到 out_chain 末尾
 *                       然后调用 write_filter 发送
 * ──────────────────────────────────────────────────────────── */
rps_int_t
rps_http_send_body(rps_http_request_t *r, rps_buf_t *body)
{
    rps_chain_t *cl, *last;

    cl = rps_palloc(r->pool, sizeof(rps_chain_t));
    if (cl == NULL) {
        return RPS_ERROR;
    }
    cl->buf  = body;
    cl->next = NULL;

    /* 追加到 out_chain 末尾 */
    if (r->out_chain == NULL) {
        r->out_chain = cl;
    } else {
        for (last = r->out_chain; last->next != NULL; last = last->next) {
            /* scan to tail */;
        }
        last->next = cl;
    }

    return rps_http_write_filter(r);
}

/* ────────────────────────────────────────────────────────────
 * rps_http_output_filter:  向后兼容包装器
 *                          将 out chain 临时设为 r->out_chain，
 *                          调用 write_filter 发送后恢复
 * ──────────────────────────────────────────────────────────── */
rps_int_t
rps_http_output_filter(rps_http_request_t *r, rps_chain_t *out)
{
    rps_chain_t *saved;

    saved = r->out_chain;
    r->out_chain = out;

    rps_int_t rc = rps_http_write_filter(r);

    r->out_chain = saved;
    return rc;
}
