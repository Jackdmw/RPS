#ifndef _RPS_THREAD_H_INCLUDED_
#define _RPS_THREAD_H_INCLUDED_

#include <pthread.h>
#include "core/rps_cycle.h"
#include "core/rps_connection.h"
#include "http/rps_http_core.h"
#include "http/rps_upstream.h"

/* 传递给线程的上下文 */
typedef struct {
    rps_cycle_t        *cycle;
    rps_connection_t   *c;            /* 客户端连接 */
    rps_http_request_t *r;            /* 已创建的空 request */
    void              **conf_ctx;     /* cycle->conf_ctx */
} rps_thread_ctx_t;

/* 线程入口 */
void *rps_thread_worker(void *arg);

/* accept 后 spawn 线程（主线程调用） */
int  rps_thread_spawn(rps_cycle_t *cycle, rps_connection_t *c,
                       rps_http_request_t *r);

/* 线程池锁初始化 / 销毁 */
int  rps_thread_mutex_init(rps_cycle_t *cycle);
void rps_thread_mutex_destroy(rps_cycle_t *cycle);

/* ── 阻塞版 upstream 操作（线程内部使用）── */

/* 阻塞 connect + send request + recv response 全流程 */
rps_int_t rps_thread_proxy_run(rps_http_request_t *r, rps_upstream_t *u);

/* WS 完整流程：connect + 握手 + poll 双向转发 */
rps_int_t rps_thread_ws_start(rps_http_request_t *r, rps_upstream_t *u);

/* WS 握手后的 poll 双向转发 */
void rps_thread_ws_forward(rps_http_request_t *r, rps_upstream_t *u);

#endif /* _RPS_THREAD_H_INCLUDED_ */
