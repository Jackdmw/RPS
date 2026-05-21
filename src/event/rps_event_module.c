#include "event/rps_event.h"
#include "core/rps_cycle.h"
#include "core/rps_core_module.h"

char *rps_event_block(rps_conf_t * cf,rps_command_t *cmd,void * conf);
void *rps_event_create_conf(rps_cycle_t *cycle);
char *rps_event_init_conf(rps_cycle_t *cycle);

rps_command_t   rps_event_root_commands[] ={
    {
        rps_string("event"),
        RPS_MAIN_CONF|RPS_CONF_NOARGS|RPS_CONF_BLOCK,
        rps_event_block,
        RPS_CONF_BELONG_CORE,
        0,
        NULL
    },
    rps_null_command
};
static rps_core_module_ctx_t rps_event_module_ctx = {
    rps_string("rps_event"),
    rps_event_create_conf,
    rps_event_init_conf
};
rps_module_t    rps_event_module ={
    -1,
    -1,
    rps_string("event"),
    "1.0.0",
    &rps_event_module_ctx,
    rps_event_root_commands,
    RPS_CORE_MODULE,
    NULL,
    NULL,
    NULL,
    NULL
};


void *rps_event_create_conf(rps_cycle_t *cycle){
    rps_event_container_t * ccf;
    rps_uint_t              i;
    rps_event_module_t     *ctx;

    ccf = rps_pcalloc(cycle -> pool, sizeof(rps_event_container_t));
    ccf -> event_conf = rps_pcalloc(cycle -> pool, sizeof(void *) * rps_event_modules_n);
    if(ccf -> event_conf == NULL){
        return NULL;
    }
    for(i = 0; cycle->modules[i];i++){
        if(cycle->modules[i]->type == RPS_EVENT_MODULE ){
            if(cycle->modules[i]->ctx != NULL){

                ctx = (rps_event_module_t*)(cycle->modules[i]->ctx);
                if(ctx -> create_conf != NULL){

                    ccf->event_conf[cycle -> modules[i] ->ctx_index] = ctx->create_conf(cycle);
                    if(ccf->event_conf[cycle -> modules[i] -> ctx_index] == NULL){
                        return NULL;
                    }
                }
            }
        }
    }

    return ccf;
}

char *rps_event_init_conf(rps_cycle_t *cycle){
    rps_event_container_t           *container;
    rps_event_conf_t                *conf;
    rps_event_module_t              *ctx;
    rps_uint_t                       i;

    container = rps_get_conf(cycle -> conf_ctx, rps_event_module);
    for( i = 0; cycle -> modules[i]; i++){
        if(cycle -> modules[i] -> type == RPS_EVENT_MODULE){
            ctx = cycle -> modules[i] -> ctx;
            if (ctx != NULL){
                if( ctx -> init_conf != NULL){
                    if(ctx -> init_conf(cycle)!=NULL){
                        return "error";
                    }
                }
            }
        }
    }
    return NULL;
}


char * rps_event_block(rps_conf_t *cf,rps_command_t * cmd,void *conf){
    rps_event_container_t   *ccf;
    rps_uint_t               i;
    rps_uint_t               j;
    rps_cycle_t             *cycle;
    rps_event_module_t      *ctx;
    rps_conf_t               old_cf;

    cycle = cf ->cycle;
    old_cf = *cf;

    ccf = conf;

    
    rps_log_error(RPS_LOG_DEBUG,cf->log,0,"prepare to execute new parse");
    
    
    cf->cmd_type = RPS_EVENT_MAIN_CONF;
    cf->ctx = ccf;

    if(rps_conf_parse(cf) == RPS_ERROR){
        rps_log_error(RPS_ERROR,cf->log,0,"parse event block failed");
        return "parse event failed";
    }

    *cf = old_cf; 
     return RPS_CONF_OK;
}
