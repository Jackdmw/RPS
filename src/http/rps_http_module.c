#include "rps_http_core.h"
#include "core/rps_core.h"
#include "core/rps_core_module.h"
#include "http/modules/rps_http_core_module.h"

static void *rps_http_module_create_conf(rps_cycle_t * cycle);
char *rps_set_http_block(rps_conf_t *cf,rps_command_t * cmd,void *conf);

rps_core_module_ctx_t rps_http_module_ctx = {
    rps_string("http_module"),
    rps_http_module_create_conf,
    NULL
};
rps_command_t   rps_http_module_commands[] = {
    {
        rps_string("http"),
        RPS_MAIN_CONF|RPS_CONF_NOARGS|RPS_CONF_BLOCK,
        rps_set_http_block,
        RPS_CONF_BELONG_CORE,
        0,
        NULL
    },
    rps_null_command
};
rps_module_t rps_http_module = {
    -1,
    -1,
    rps_string("http_module"),
    "1.0.0",
    &rps_http_module_ctx,
    rps_http_module_commands,
    RPS_CORE_MODULE,
    NULL,
    NULL,
    NULL,
    NULL
};
/**
 * http 块初始化只创建容器，模块的配置挂载由块指令具体执行
 */
void *rps_http_module_create_conf(rps_cycle_t * cycle){
    rps_http_conf_container_t           *container;
    rps_uint_t                           i;
    rps_http_module_t                   *ctx;

    container = rps_palloc(cycle -> pool, sizeof(rps_http_conf_container_t));
    if(container == NULL){
        return NULL;
    }
    container -> loc_conf = rps_pcalloc(cycle -> pool, sizeof(void *) * rps_http_modules_n);
    container -> main_conf = rps_pcalloc(cycle -> pool, sizeof(void *) * rps_http_modules_n);
    container -> srv_conf = rps_pcalloc(cycle -> pool, sizeof(void *) * rps_http_modules_n);

    if(container -> loc_conf == NULL || container -> main_conf == NULL|| container -> srv_conf == NULL){
        return NULL;
    }

    return container;
}
/**
 * conf 从parse传进来的是container的地址，
 * 根容器的指针数组是创建好了的
 */
char *rps_set_http_block(rps_conf_t *cf,rps_command_t * cmd,void *conf){
    rps_http_conf_container_t       *container;
    rps_uint_t                       i;
    rps_conf_t                       old_cf;
    rps_cycle_t                     *cycle;
    rps_module_t                   **modules;
    rps_http_module_t               *ctx;
    rps_http_core_main_conf_t       *cmcf;

    old_cf = *cf;
    container = conf;
    cycle = cf -> cycle;
    modules = cycle -> modules;

    for ( i = 0; modules[i]; i++){
        if (modules[i]->type == RPS_HTTP_MODULE){
            ctx = modules[i] -> ctx;
            if (ctx != NULL){
                if(ctx -> create_loc_conf != NULL){
                    container -> loc_conf [modules[i] -> ctx_index] = ctx -> create_loc_conf(cf);
                    if (container -> loc_conf[modules[i] -> ctx_index] == NULL){
                        return "error in init container of http";
                    }
                }
                if (ctx -> create_main_conf != NULL){
                    container -> main_conf[modules[i] -> ctx_index] = ctx -> create_main_conf (cf);
                    if( container -> main_conf[modules[i] -> ctx_index] == NULL){
                        return "error in init container of http";
                    }
                }
                if (ctx -> create_srv_conf != NULL){
                    container -> srv_conf[modules[i] -> ctx_index] = ctx -> create_srv_conf(cf);
                    if (container -> srv_conf[modules[i] -> ctx_index] == NULL){
                        return "error in init container of http";
                    }
                }
            }
        }
    }

    cf -> cmd_type = RPS_HTTP_MAIN_CONF;
    cf -> ctx = container;
    if (rps_conf_parse(cf) == RPS_ERROR){
        rps_log_error(RPS_LOG_ERR,cycle -> log, 0, "parse http block failed");
        return "parse error";
    }

    /*
     * 配置解析完成，调用各 HTTP 模块的 postconfiguration 钩子
     * 此时模块可注册 phase handler
     * core 模块的钩子还同时执行了配置合并， 后面不用加了
     */
    for (i = 0; modules[i]; i++){
        if (modules[i]->type == RPS_HTTP_MODULE){
            ctx = modules[i] -> ctx;
            if (ctx != NULL && ctx -> postconfiguration != NULL){
                if (ctx -> postconfiguration(cf) != RPS_OK){
                    rps_log_error(RPS_LOG_ERR, cycle -> log, 0,
                                  "postconfiguration of module %s failed",
                                  modules[i]->name.data);
                    return "postconfiguration failed";
                }
            }
        }
    }
    cmcf = container -> main_conf[rps_http_core_module.ctx_index];
    rps_log_error(RPS_LOG_INFO, cycle -> log, 0, "prepare to init phases engine");
        /* 初始化阶段引擎 */
    if (rps_http_init_phase_engine(cmcf) != RPS_OK) {
        return "phase_engine_init_failed!";
    }
    *cf = old_cf;
    return RPS_CONF_OK;
}