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
    rps_buf_t                    b;
    rps_conf_file_t              conf_file;
    void                        *ctx;

    log = old_cycle->log;


    pool = rps_create_pool(1024);
    if (pool == NULL) {
        rps_log_error(RPS_LOG_EMERG,log,0,"[file]:%s [LINE]: %d 创建cycle内存池失败",__FILE__,__LINE__);
    }
    rps_log_error(RPS_LOG_DEBUG,log,0,"the new pool of new cycle has been created!");
    pool -> log = log;

    cycle = (rps_cycle_t*)rps_pcalloc(pool,sizeof(rps_cycle_t));
    if (cycle == NULL) {
        rps_destroy_pool(pool);
        return NULL;
    }
    rps_log_error(RPS_LOG_DEBUG,log,0,"New cycle's memory has been malloc successfully ");
    
    
    cycle -> pool = pool;
    cycle -> log = log;
    cycle -> conf_file = old_cycle -> conf_file;
    if(rps_array_init(&cycle->listening,pool,10,sizeof(rps_listening_t)) != RPS_OK){
        return NULL;
    }
    if(rps_list_init(&cycle->open_files,pool,20,sizeof(rps_open_file_t)) != RPS_OK){
        rps_destroy_pool(pool);
        return NULL;
    }

    cycle->conf_ctx = rps_pcalloc(pool, sizeof(void *) * rps_max_module);
    if (cycle->conf_ctx == NULL) { 
        rps_destroy_pool(pool);
        return NULL;
    }
    for ( i = 0; i < rps_max_module; i++){
        cycle -> conf_ctx[i] = NULL;
    }
    
    cycle->modules = old_cycle -> modules;
    cycle -> modules_n = old_cycle -> modules_n;


    for (i = 0; cycle->modules[i]; i++)
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
            rps_log_error(RPS_LOG_DEBUG,log,0,"core module %s conf has been created",cycle -> modules[i]->name.data);
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
    conf.cmd_type = RPS_MAIN_CONF;
    conf.cycle = cycle;
    conf.conf_file = &conf_file;
    if(rps_buf_init(conf.pool,&b,4096)==RPS_ERROR){
        rps_log_error(RPS_LOG_ERR,log,0,"buf allocate failed");
        return NULL;
    }
    
    
    conf.conf_file->file.fd = open(cycle->conf_file.data,O_RDONLY);
    if (conf.conf_file->file.fd == RPS_INVALID_FILE) {
        rps_log_error(RPS_LOG_ERR, log, 0, "failed to open config file \"%s\"",
                      cycle->conf_file.data);
        rps_destroy_pool(pool);
        return NULL;
    }
    conf.conf_file->file.name.len = cycle->conf_file.len;
    conf.conf_file->file.name.data = cycle->conf_file.data;
    conf.conf_file->file.offset = 0;
    conf.conf_file->file.log = log;
    conf.conf_file->line = 1;
    conf.file_name = cycle->conf_file;

    conf.conf_file->buffer = &b;

    rps_log_error(RPS_LOG_DEBUG,log,0,"conf parse start,file:%s",cycle->conf_file.data);
    if(rps_conf_parse(&conf)==RPS_ERROR){
        close(conf.conf_file->file.fd);
        rps_destroy_pool(pool);
        return NULL;
    }
    rps_log_error(RPS_LOG_DEBUG,log,0,"Parse successfully");

    for (i = 0; i < cycle -> modules_n; i++){
        if (cycle -> modules[i] -> type != RPS_CORE_MODULE)
            continue;
        module = cycle -> modules[i] -> ctx;
        if (module->init_conf != NULL){
            if(module -> init_conf(cycle)!=NULL){
                close(conf.conf_file->file.fd);
                rps_destroy_pool(pool);
                rps_log_error(RPS_LOG_ERR,log,0,"init core module failed");
                return NULL;
            }
        } 
    }

    close(conf.conf_file->file.fd);
    return cycle;
}