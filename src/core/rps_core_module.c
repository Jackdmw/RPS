#include "rps_core_module.h"
#include  "rps_core.h"
#include "rps_cycle.h"
#include "rps_conf_file.h"

static rps_command_t rps_core_commands[] = {
    {   rps_string("worker_processes"),
        RPS_MAIN_CONF|RPS_CONF_TAKE1,
        rps_set_worker_processes,
        0,offsetof(rps_core_conf_t,worker_processes),NULL
    },
    rps_null_command
};

static rps_core_module_ctx_t rps_core_module_ctx = {
    rps_string("core"),
    rps_core_create_conf,
    rps_core_init_conf
};

rps_module_t  rps_core_module = {
    0,
    0,
    rps_string("core"),
    "1.0.0",
    &rps_core_module_ctx,
    rps_core_commands,
    RPS_CORE_MODULE,
    NULL,
    NULL,
    NULL,
    NULL
};

void *rps_core_create_conf(rps_cycle_t * cycle){
    rps_core_conf_t  *ccf;
    ccf = rps_pcalloc(cycle->pool,sizeof(rps_core_conf_t));
    if(ccf == NULL) return NULL;

    ccf->worker_processes  =  RPS_CONF_UNSET_UINT;
    return ccf;
}

char *rps_set_worker_processes(){
    
}