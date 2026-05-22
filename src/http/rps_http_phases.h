#ifndef _RPS_HTTP_PHASES_H_
#define _RPS_HTTP_PHASES_H_

#include "core/rps_config.h"
#include "core/rps_array.h"
#include "http/rps_http_core.h"
#include "http/modules/rps_http_core_module.h"
// 阶段定义
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
typedef rps_int_t
(*rps_http_phase_handler_pt)(rps_http_request_t *r, rps_http_phase_handler_t *ph);

typedef struct {
    rps_array_t     handlers;// element type: rps_http_handler_pt
} rps_http_phase_t;

struct rps_http_phase_handler_s{

    rps_http_phase_handler_pt checker;

    rps_http_handler_pt       handler;

    rps_uint_t                next;

} ;
typedef struct {
    rps_http_phase_handler_t  *handlers;
    rps_uint_t                 server_rewrite_index;
    rps_uint_t                 location_rewrite_index;
} rps_http_phase_engine_t;

// 注册阶段 handler（由 http_core 的 postconfiguration 调用）
void rps_http_register_phase_handler(rps_uint_t phase, rps_http_handler_pt handler);
// 阶段引擎
rps_int_t rps_http_run_phases(rps_http_request_t *r);
rps_int_t rps_http_init_phase_engine(rps_http_core_main_conf_t *cmcf);
#endif