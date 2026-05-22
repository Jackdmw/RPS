#ifndef _RPS_HTTP_PHASES_H_
#define _RPS_HTTP_PHASES_H_

#include "core/rps_config.h"
#include "core/rps_array.h"
#include "http/rps_http_core.h"

typedef struct rps_http_core_main_conf_s rps_http_core_main_conf_t;

/*
 * Handler 返回到 checker 的值
 */
#define RPS_HTTP_OK         RPS_OK          /*  0: 当前 handler 处理成功        */
#define RPS_HTTP_ERROR      RPS_ERROR       /* -1: 发生致命错误                  */
#define RPS_HTTP_AGAIN      RPS_AGAIN       /* -4: IO 未就绪，挂起等待事件        */
#define RPS_HTTP_DECLINED   RPS_DECLINED    /* -3: handler 不处理，交给下一个     */
#define RPS_HTTP_DONE       -5              /*     请求最终处理完毕               */

/* 阶段定义 */
#define RPS_HTTP_PHASE_NUM  11
typedef enum {
    RPS_HTTP_POST_READ_PHASE = 0,
    RPS_HTTP_SERVER_REWRITE_PHASE,
    RPS_HTTP_FIND_CONFIG_PHASE,
    RPS_HTTP_REWRITE_PHASE,
    RPS_HTTP_POST_REWRITE_PHASE,
    RPS_HTTP_PREACCESS_PHASE,
    RPS_HTTP_ACCESS_PHASE,
    RPS_HTTP_POST_ACCESS_PHASE,
    RPS_HTTP_PRECONTENT_PHASE,
    RPS_HTTP_CONTENT_PHASE,
    RPS_HTTP_LOG_PHASE,
} rps_http_phases;

typedef rps_int_t (*rps_http_handler_pt)(rps_http_request_t *r);
typedef struct rps_http_phase_handler_s rps_http_phase_handler_t;

/*
 * checker 签名：负责流程控制（调用 handler，根据返回值更新 phase_index）
 * handler 签名：纯业务逻辑，不关心跳转
 */
typedef rps_int_t
(*rps_http_phase_handler_pt)(rps_http_request_t *r, rps_http_phase_handler_t *ph);

typedef struct {
    rps_array_t     handlers;   /* rps_http_handler_pt */
} rps_http_phase_t;

struct rps_http_phase_handler_s {
    rps_http_phase_handler_pt checker;
    rps_http_handler_pt       handler;
    rps_uint_t                next;    /* 当前 handler 所属 phase 执行完毕后跳到哪个下标 */
};

typedef struct {
    rps_http_phase_handler_t  *handlers;               /* 展平后的一维 handler 数组            */
    rps_uint_t                 server_rewrite_index;   /* FIND_CONFIG 的首个 handler 下标      */
    rps_uint_t                 location_rewrite_index; /* 同上，供 POST_REWRITE 跳回用          */
} rps_http_phase_engine_t;

/*
 * 注册阶段 handler：postconfiguration 阶段由模块调用，
 * 把自己的 handler 函数指针推入对应 phase 的数组中
 */
#define rps_http_register_phase_handler(phase, handler, cmcf) {  \
    rps_http_handler_pt  *__new;                                 \
    __new = rps_array_push(&(cmcf)->phases[phase].handlers);     \
    if (__new != NULL) *__new = (handler);                       \
}

/* 阶段引擎 */
rps_int_t rps_http_run_phases(rps_http_request_t *r, rps_http_core_main_conf_t *cmcf);
rps_int_t rps_http_init_phase_engine(rps_http_core_main_conf_t *cmcf);
#endif
