#include "rps_cycle.h"
#include "rps_file.h"
extern rps_module_t  *rps_modules[];
extern rps_uint_t     rps_max_module;

rps_cycle_t *
rps_init_cycle(rps_cycle_t *old_cycle){

    rps_uint_t                   i;
    rps_pool_t                  *pool;
    rps_cycle_t                 *cycle;
    rps_log_t                   *log;
    void                        *ctx;

    log = old_cycle->log;


    pool = rps_create_pool(1024);
    if (pool == NULL) {
        rps_log_error(RPS_LOG_EMERG,log,0,"[file]:%s [LINE]: %d 创建cycle内存池失败",__FILE__,__LINE__);
    }

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
        return NULL;
    }

    cycle->conf_ctx = rps_pcalloc(pool, sizeof(void *) * rps_max_module);
    if (cycle->conf_ctx == NULL) {
        return NULL;
    }

}