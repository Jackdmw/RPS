#include "rps_http_core.h"
#include "rps_http_phases.h"
#include "core/rps_connection.h"
#include "core/rps_palloc.h"
#include "event/rps_event.h"

/*
 * 创建 HTTP 请求对象及独立内存池
 */
rps_http_request_t *
rps_http_create_request(rps_connection_t *c)
{
    rps_http_request_t          *request;
    rps_pool_t                  *pool;

    pool = rps_create_pool(4096);
    if (pool == NULL) {
        return NULL;
    }

    request = rps_pcalloc(pool, sizeof(rps_http_request_t));
    if (request == NULL) {
        rps_destroy_pool(pool);
        return NULL;
    }
    request->connection = c;
    request->pool       = pool;
    request->cycle      = c->cycle;

    request->method       = (rps_str_t)rps_null_string;
    request->uri          = (rps_str_t)rps_null_string;
    request->args         = (rps_str_t)rps_null_string;
    request->http_version = (rps_str_t)rps_null_string;
    request->host         = (rps_str_t)rps_null_string;

    rps_memzero(&request->headers_in,  sizeof(rps_http_headers_in_t));
    rps_memzero(&request->headers_out, sizeof(rps_http_headers_out_t));

    request->headers_in.connection.key     = (rps_str_t)rps_string("connection");
    request->headers_in.content_length.key = (rps_str_t)rps_string("content-length");
    request->headers_in.user_agent.key     = (rps_str_t)rps_string("user-agent");
    request->headers_in.content_type.key   = (rps_str_t)rps_string("content-type");
    request->headers_in.upgrade.key        = (rps_str_t)rps_string("upgrade");

    request->headers_out.content_type.key = (rps_str_t)rps_string("content-type");
    request->headers_out.server.key       = (rps_str_t)rps_string("server");
    request->headers_out.status.key       = (rps_str_t)rps_string("status");

    if (rps_list_init(&request->headers_in.headers, pool, 5,
                      sizeof(rps_http_header_kv_t)) == RPS_ERROR) {
        rps_destroy_pool(pool);
        return NULL;
    }
    request->headers_in.headers_n = 0;

    if (rps_list_init(&request->headers_out.headers, pool, 5,
                      sizeof(rps_http_header_kv_t)) == RPS_ERROR) {
        rps_destroy_pool(pool);
        return NULL;
    }
    request->headers_out.headers_n = 0;

    request->headers_out.status.value           = (rps_str_t)rps_string("200 OK");
    request->headers_out.server.value           = (rps_str_t)rps_string("RPS");
    request->headers_out.content_type.value     = (rps_str_t)rps_null_string;
    request->headers_out.content_length_n.value = (rps_str_t)rps_null_string;

    request->request_body = rps_buf_create(pool, 4096);
    if (request->request_body == NULL) {
        rps_destroy_pool(pool);
        return NULL;
    }
    request->request_body_rest = 0;
    request->reading_body      = 0;

    request->out_chain   = NULL;
    request->upstream    = NULL;
    request->parse_status = 0;
    request->loc_conf     = NULL;
    request->main_conf    = NULL;
    request->srv_conf     = NULL;
    request->uri_changed  = 0;
    request->internal_redirect = 0;
    request->start_msec = rps_current_msec();
    request->keepalive   = 1;
    request->log = c->listenling ? c->listenling->log : NULL;

    /* 每次创建 request 重置客户端读超时，keepalive 连接复用时不继承旧计时 */
    if (c->read) {
        rps_event_add_timer(c->read, 60000);
    }

    rps_log_error(RPS_LOG_DEBUG, request -> log, 0, "new request has been created");
    return request;
}

/*
 * 释放请求资源（upstream + pool）
 */
void
rps_http_release_request(rps_http_request_t *r)
{
    rps_pool_t *pool;

    pool = r->pool;

    /* 清理可能正在等待 EAGAIN 的写事件，防止引用已释放的 pool */
    if (r->connection && r->connection->write
        && r->connection->write->active)
    {
        r->cycle->event_engine->del_event(r->connection->write,
                                          RPS_WRITE_EVENT);
        r->connection->write->active = 0;
        rps_event_del_timer(r->connection->write);
    }

    /*
     * 正常流程下 r->upstream 已在 rps_upstream_finalize 中置 NULL。
     * 若仍未释放（异常路径），清理其后端连接后丢弃。
     */
    if (r->upstream) {
        if (r->upstream->peer) {
            rps_upstream_close_peer_conn(r->upstream->peer);
            r->upstream->peer = NULL;
        }
        r->upstream = NULL;
    }

    if (r->connection && r->connection->data == r) {
        r->connection->data = NULL;
    }

    if (pool) {
        rps_destroy_pool(pool);
    }
}

/*
 * 结束请求：根据 keepalive + 成功/失败 决定连接命运。
 * 销毁 r，调用者不可再访问。
 * 请求结束后，应当立即决定连接是否存活
 */
void
rps_http_finalize_request(rps_http_request_t *r, rps_int_t rc)
{
    rps_connection_t  *c;
    unsigned           keepalive;

    c         = r->connection;
    keepalive = r->keepalive;

    if (rc != RPS_OK) {
        keepalive = 0;
    }

    rps_http_release_request(r);
    rps_log_error(RPS_LOG_DEBUG,c -> cycle -> log, 0, "release http request");
    if (c) {
        c->close = !keepalive;
    }
}

/** 
 * 请求已结束 (finalize 后调用)，决定连接的命运。
 * 根据c -> close,决定是否释放连接，以及是否创建新的请求对象
 */
void
rps_http_complete_request(rps_connection_t *c)
{
    rps_http_request_t  *r;

    if (c->close) {
        rps_free_connection(c);
        return;
    }

    r = rps_http_create_request(c);
    if (r == NULL) {
        rps_free_connection(c);
        return;
    }
    c->data          = r;
    c->read->handler = rps_http_wait_request_handler;
    c->read->data    = r; c->read->connection = c;

    if (c->cycle->event_engine->add_event(c->read, RPS_READ_EVENT) != RPS_OK) {
        rps_free_connection(c);
        return;
    }
    /* 计时器已在 rps_http_create_request 中重置 */
}
