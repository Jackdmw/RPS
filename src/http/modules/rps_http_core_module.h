#ifndef _RPS_HTTP_CORE_MODULE_H_
#define _RPS_HTTP_CORE_MODULE_H_

#include "core/rps_array.h"
#include "core/rps_config.h"
#include "http/rps_http_phases.h"

typedef struct rps_http_core_main_conf_s {
    rps_array_t                 servers;       // rps_http_conf_container_t *
    rps_uint_t                  client_max_body_size;
    rps_http_phase_t            phases[RPS_HTTP_PHASE_NUM];
    rps_http_phase_engine_t     phase_engine;
    rps_array_t                 upstreams;     /* rps_upstream_conf_t* */
} rps_http_core_main_conf_t;

// server {} 级配置
typedef struct {
    rps_str_t      server_name;
    rps_uint_t     port;
    rps_array_t    locations;
    /* listen 创建的 listening，由 postconfiguration 阶段处理 */
} rps_http_core_srv_conf_t;

// location {} 级配置
typedef struct {
    rps_str_t      pattern;      // location 后面的路径，如 /api/
    rps_uint_t     exact_match:1;// = /pattern 精确匹配
} rps_http_core_loc_conf_t;


#endif