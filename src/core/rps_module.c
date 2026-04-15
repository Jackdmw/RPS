#include "rps_core.h"
#include "rps_module.h"



#define RPS_MAX_DYNAMIC_MODULES    128

rps_module_t *rps_modules[] = {
    &rps_core_module,
    NULL
};
rps_uint_t      rps_max_module;
static rps_uint_t rps_modules_n;

rps_int_t
rps_preinit_modules(void)
{
    rps_uint_t  i;

    for (i = 0; rps_modules[i];i++){
        rps_modules[i] -> index =i;
    }

    rps_modules_n  = i;
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