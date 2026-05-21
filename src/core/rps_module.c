#include "rps_core.h"
#include "rps_module.h"
#include "rps_core_module.h"
#include "event/rps_event.h"
#include "http/rps_http_core.h"

#define RPS_MAX_DYNAMIC_MODULES    128

rps_module_t *rps_modules[] = {
    &rps_core_module,
    &rps_event_core_module,
    &rps_event_module,
    &rps_http_module,
    &rps_http_core_module,
    NULL
};
rps_uint_t      rps_max_module;
rps_uint_t      rps_modules_n;
rps_uint_t      rps_event_modules_n;
rps_uint_t      rps_http_modules_n;

rps_int_t
rps_preinit_modules(rps_log_t *log)
{
    rps_uint_t  i;
    rps_uint_t  event_ctx_index;
    rps_uint_t  http_ctx_index;

    for (i = 0,event_ctx_index = 0, http_ctx_index = 0; rps_modules[i];i++){
        rps_modules[i] -> index =i;
        if(rps_modules[i]->type == RPS_EVENT_MODULE){
            rps_modules[i]->ctx_index = event_ctx_index;
            event_ctx_index ++;
        }
        else if (rps_modules[i]->type == RPS_HTTP_MODULE){
            rps_modules[i]->ctx_index = http_ctx_index;
            http_ctx_index ++;
        }
    }

    rps_modules_n  = i;
    rps_event_modules_n = event_ctx_index;
    rps_http_modules_n = http_ctx_index;
    rps_max_module = rps_modules_n + RPS_MAX_DYNAMIC_MODULES;

    return RPS_OK;
}

rps_int_t
rps_init_modules(rps_cycle_t *cycle)
{
    rps_uint_t  i;

    for(i = 0; cycle->modules[i]; i++){
        if (cycle->modules[i]->init_module){
            if(cycle->modules[i]->init_module(cycle)!=RPS_OK){
                return RPS_ERROR;
            }
        }
    }
    return RPS_OK;
}