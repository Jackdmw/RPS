#include "rps_cycle.h"
#include "rps_file.h"
#include "rps_conf_file.h"
#include "rps_core_module.h"
extern rps_module_t  *rps_modules[];
extern rps_uint_t     rps_max_module;

rps_cycle_t *
rps_init_cycle(rps_cycle_t *old_cycle){

    rps_uint_t                   i;
    rps_pool_t                  *pool;
    rps_cycle_t                 *cycle;
    rps_log_t                   *log;
    rps_conf_t                   conf;
    rps_core_module_ctx_t       *module;  
    void                        *ctx;

    log = old_cycle->log;


    pool = rps_create_pool(1024);
    if (pool == NULL) {
        rps_log_error(RPS_LOG_EMERG,log,0,"[file]:%s [LINE]: %d 创建cycle内存池失败",__FILE__,__LINE__);
    }
    pool->log = log;
    cycle = (rps_cycle_t*)rps_pcalloc(pool,sizeof(rps_cycle_t));
    if (cycle == NULL) {
        rps_destroy_pool(pool);
        return NULL;
    }

    cycle -> pool = pool;
    cycle -> log = log;

    // if(rps_array_init(&cycle->listening,pool,10,sizeof(rps_listening_t)) != RPS_OK){
    //     return NULL;
    // }

    if(rps_list_init(&cycle->open_files,pool,20,sizeof(rps_open_file_t)) != RPS_OK){
        rps_destroy_pool(pool);
        return NULL;
    }

    cycle->conf_ctx = rps_pcalloc(pool, sizeof(void *) * rps_max_module);
    if (cycle->conf_ctx == NULL) {
        rps_destroy_pool(pool);
        return NULL;
    }
    cycle->modules = old_cycle -> modules;
    for (int i =0; cycle->modules[i]; i++)
    {
        if (cycle->modules[i]->type != RPS_CORE_MODULE)
            continue;

        module = cycle->modules[i]->ctx;
        if(module->create_conf){
            void * rc = module->create_conf(cycle);
            if(rc == NULL)
            {
                rps_destroy_pool(pool);
                return NULL;
            }
            cycle->conf_ctx[cycle->modules[i]->index]=rc;
        }
    }

    conf.args = rps_array_create(pool,10,sizeof(rps_str_t));
    if(conf.args == NULL)
    {
        rps_destroy_pool (pool);
        return NULL;
    }

    conf.ctx = cycle -> conf_ctx;
    conf.pool = cycle -> pool;
    conf.log = cycle -> log;
    conf.module_type = RPS_CORE_MODULE;
    conf.cycle = cycle;

}