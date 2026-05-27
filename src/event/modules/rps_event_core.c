#include "event/rps_event.h"
#include "core/rps_cycle.h"
char *rps_set_use(rps_conf_t *cf,rps_command_t *cmd,void* conf);

void * rps_event_core_create_conf(rps_cycle_t *cycle);
char  *rps_event_core_init_conf(rps_cycle_t *cycle);
rps_int_t  rps_event_core_init_process(rps_cycle_t *cycle);

rps_command_t rps_event_core_commands[] = {
    
    {
        rps_string("use"),
        RPS_EVENT_MAIN_CONF|RPS_CONF_TAKE1,
        rps_set_use,
        RPS_CONF_BELONG_EVENT,
        offsetof(rps_event_conf_t,use),
        NULL
    },
    {
        rps_string("worker_connections"),
        RPS_EVENT_MAIN_CONF|RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_EVENT,
        offsetof(rps_event_conf_t,worker_connections),
        NULL
    },
    rps_null_command
};

static rps_event_module_t rps_event_core_module_ctx = {
    rps_event_core_create_conf,
    rps_event_core_init_conf
};

rps_module_t rps_event_core_module = {
    -1,
    -1,
    rps_string("event_core"),
    "1.0.0",
    &rps_event_core_module_ctx,
    rps_event_core_commands,
    RPS_EVENT_MODULE,
    NULL,
    rps_event_core_init_process,
    NULL,
    NULL,
};

void *rps_event_core_create_conf(rps_cycle_t * cycle){
    rps_event_conf_t * ccf;
    ccf = rps_pcalloc(cycle->pool,sizeof(rps_event_conf_t));
    if (ccf == NULL) return NULL;

    ccf -> use = (rps_str_t)rps_string("epoll");
    ccf ->worker_connections = RPS_CONF_UNSET_UINT;

    return ccf;
}


char * rps_set_use(rps_conf_t *cf,rps_command_t *cmd,void* conf){
    rps_str_t           *values;
    rps_event_conf_t    *ccf;

    ccf = conf;
    values = cf ->args ->elts;

    if(rps_strcmp_with_cstr(values[1],"epoll")){
        ccf -> use = (rps_str_t)rps_string("epoll");
        cf -> cycle -> if_pthread = 0;
    }
    else if (rps_strcmp_with_cstr(values[1],"io_uring")){
        ccf -> use = (rps_str_t)rps_string("io_uring");
    }
    else if ( rps_strcmp_with_cstr(values[1],"threads")){
        ccf -> use = (rps_str_t)rps_string("threads");
        cf -> cycle -> if_pthread = 1;
    }
    else {
        rps_log_error(RPS_LOG_ERR,cf->log,0,"command \"use\" 's attribute could only in \"epoll\" or \"io_uring\"!");
        return "attribute not well";
    }
    return RPS_CONF_OK;
}
/**
 * 初始化全局连接池，以及配套的读写事件池
 */
rps_int_t  rps_event_core_init_process(rps_cycle_t *cycle){
    rps_int_t                i;
    rps_module_t           **modules;
    rps_event_conf_t        *conf;
    rps_event_container_t   *container;
    rps_uint_t               worker_connections;

    modules = cycle -> modules;
    
    container = rps_get_conf(cycle -> conf_ctx,rps_event_module);
    
    conf = container -> event_conf[rps_event_core_module.ctx_index];

    worker_connections = conf -> worker_connections;

    cycle -> connections = rps_palloc(cycle -> pool, sizeof(rps_connection_t) * worker_connections);
    if (cycle -> connections == NULL){
        return RPS_ERROR;
    }

    cycle -> reads = rps_palloc(cycle -> pool, sizeof(rps_event_t) * worker_connections);
    if (cycle -> reads == NULL){
        return RPS_ERROR;
    }
    cycle -> writes = rps_palloc(cycle -> pool, sizeof(rps_event_t) * worker_connections);
    if (cycle -> writes == NULL){
        return RPS_ERROR;
    }

    for( i = 0; i < worker_connections - 1; i++){
        cycle -> connections[i].data = cycle -> connections + (i+1);
        cycle -> connections[i].read = cycle -> reads + i;
        cycle -> connections[i].write = cycle -> writes + i;
        cycle -> connections[i].read->data = &cycle -> connections[i];
        cycle -> connections[i].write->data = &cycle -> connections[i];

        cycle -> connections[i].cycle = cycle;
        cycle -> connections[i].sockaddr = rps_pcalloc(cycle -> pool, sizeof(struct sockaddr));
        if(cycle -> connections[i].sockaddr == NULL){
            return RPS_ERROR;
        }
    }
    cycle -> connections [worker_connections-1].data = NULL;
    cycle -> free_connection = cycle -> connections;
    return RPS_OK;
}

char  *rps_event_core_init_conf(rps_cycle_t *cycle){
    rps_event_container_t           *container;
    rps_module_t                   **modules;
    rps_event_module_t              *ctx;
    rps_uint_t                       i;
    rps_uint_t                       j;
    rps_event_conf_t                *conf;
    

    modules = cycle -> modules;
    container = rps_get_conf(cycle -> conf_ctx, rps_event_module);
    
    // /**
    //  * 容器未创建
    //  */
    // if (container == NULL){
    //     container = rps_pcalloc(cycle -> pool, sizeof(rps_event_container_t));
    //     container -> event_conf = rps_pcalloc(cycle -> pool, sizeof(void *) * rps_event_modules_n);
    //     if (container == NULL){
    //         return "create container falied";
    //     }
    //     for (i = 0; i < cycle -> modules_n; i++){
    //         if (modules[i]->type == RPS_EVENT_MODULE){
    //             ctx = (rps_event_module_t *)modules[i]->ctx;
    //             if (ctx -> create_conf != NULL){
    //                 container ->event_conf[modules[i]->ctx_index] = ctx -> create_conf(cycle);
    //                 if(container -> event_conf[modules[i] -> ctx_index] == NULL){
    //                     return "create conf failde";
    //                 }
    //             }
    //         }
    //     }
    //     cycle -> conf_ctx[rps_event_core_module.index] = container;
    // }
    conf = container -> event_conf[rps_event_core_module.ctx_index];
    
    rps_conf_init_value(conf -> worker_connections,10);
    return NULL;
}