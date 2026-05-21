#include "http/rps_http_core.h"
#include "core/rps_conf_file.h"
#include "http/modules/rps_http_core_module.h"
#include "core/rps_module.h"

char *rps_set_server_block(rps_conf_t *cf,rps_command_t *cmd,void *conf);
char *rps_set_server_name(rps_conf_t *cf,rps_command_t *cmd,void *conf);
char *rps_set_location_block(rps_conf_t *cf,rps_command_t *cmd,void *conf);

void *rps_http_core_create_main_conf(rps_conf_t * cf);
void *rps_http_core_create_srv_conf(rps_conf_t *cf);
void *rps_http_core_create_loc_conf(rps_conf_t *cf);

char *rps_http_core_merge_srv_conf(rps_pool_t *pool, void *parent, void *child);
char *rps_http_core_merge_loc_conf(rps_pool_t *pool, void *parent, void *child);

rps_command_t rps_http_core_module_commands[] = {
    
    {
        rps_string("server"),
        RPS_HTTP_MAIN_CONF|RPS_CONF_NOARGS|RPS_CONF_BLOCK,
        rps_set_server_block,
        RPS_CONF_BELONG_HTTP_MAIN,
        0,
        NULL
    },
    {
        rps_string("listen"),
        RPS_HTTP_SRV_CONF|RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_HTTP_SRV,
        offsetof(rps_http_core_srv_conf_t,port),
        NULL
    },
    {
        rps_string("server_name"),
        RPS_HTTP_SRV_CONF|RPS_CONF_TAKE1,
        rps_set_server_name,
        RPS_CONF_BELONG_HTTP_SRV,
        offsetof(rps_http_core_srv_conf_t,server_name),
        NULL
    },
    {
        rps_string("location"),
        RPS_HTTP_SRV_CONF|RPS_CONF_TAKE1|RPS_CONF_BLOCK,
        rps_set_location_block,
        RPS_CONF_BELONG_HTTP_SRV,
        0,
        NULL
    },
    {
        rps_string("http_body_max_size"),
        RPS_HTTP_MAIN_CONF|RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_HTTP_MAIN,
        offsetof(rps_http_core_main_conf_t,client_max_body_size),
        NULL
    },
    rps_null_command
};

static rps_http_module_t rps_http_core_module_ctx = {
    NULL,       /**preconfiguration */
    NULL,       /**postconfig */

    rps_http_core_create_main_conf,
    rps_http_core_create_srv_conf,
    rps_http_core_create_loc_conf,

    rps_http_core_merge_srv_conf,
    rps_http_core_merge_loc_conf
};

rps_module_t   rps_http_core_module = {
    -1,
    -1,
    rps_string("http_core"),
    "1.0.0",
    &rps_http_core_module_ctx,
    rps_http_core_module_commands,
    RPS_HTTP_MODULE,
    NULL,
    NULL,
    NULL,
    NULL
};


char *rps_set_server_block(rps_conf_t *cf,rps_command_t *cmd,void *conf){
    rps_http_core_main_conf_t               *ccf;
    rps_cycle_t                             *cycle;
    rps_http_conf_container_t               *new_srv;
    rps_uint_t                               i;
    rps_module_t                           **modules;
    rps_http_conf_container_t               *root_container;
    rps_http_module_t                       *ctx;
    rps_conf_t                               old_cf;
    void                                   **array_ele;
    

    old_cf = *cf;
    root_container = cf -> ctx;
    ccf = conf;
    cycle = cf -> cycle;
    modules = cycle -> modules;

    /**
     * 
     */
    new_srv = rps_palloc(cf -> pool, sizeof(rps_http_conf_container_t));
    array_ele = rps_array_push(&ccf -> servers);
    if (new_srv == NULL || array_ele == NULL){
        rps_log_error(RPS_LOG_ERR, cf -> log, 0, "palloc srv instance failed");
        return "palloc error";
    }
    *array_ele = new_srv; 
    new_srv -> loc_conf = rps_palloc(cf -> pool, sizeof(void *) *rps_http_modules_n);
    new_srv -> srv_conf = rps_palloc(cf -> pool, sizeof(void *) *rps_http_modules_n);
    /**
     * 继承main
     */
    new_srv -> main_conf = root_container -> main_conf;

    if (new_srv -> loc_conf == NULL || new_srv -> srv_conf == NULL){
        rps_log_error(RPS_LOG_ERR, cf -> log, 0, "palloc srv instance's  failed");
        return "error";
    }


    for (i = 0; modules[i] ; i++){
        if (modules[i]->type == RPS_HTTP_MODULE){
            ctx = modules[i]->ctx;
            if (ctx != NULL){
                if(ctx -> create_srv_conf != NULL){
                    new_srv -> srv_conf[modules[i] -> ctx_index] = ctx -> create_srv_conf(cf);
                    if(new_srv -> srv_conf[modules[i] -> ctx_index] == NULL){
                        rps_log_error(RPS_LOG_DEBUG, cycle -> log, 0, "new container init failed");
                        return "palloc failed";
                    }
                }
                if (ctx -> create_loc_conf != NULL){
                    new_srv -> loc_conf[modules[i] -> ctx_index] = ctx -> create_loc_conf(cf);
                    if(new_srv -> loc_conf[modules[i] -> ctx_index] == NULL){
                        rps_log_error(RPS_LOG_DEBUG, cycle -> log, 0, "new container init failed");
                        return "palloc failed";
                    }
                }
            }
        }
    }
    cf -> cmd_type = RPS_HTTP_SRV_CONF;
    cf -> ctx = new_srv;
    rps_log_error(RPS_LOG_DEBUG,cycle -> log, 0, "prepare to parse http server block");
    if (rps_conf_parse(cf) == RPS_ERROR){
        return "parse server error";
    }
    *cf = old_cf;
    return RPS_CONF_OK;
}
char *rps_set_server_name(rps_conf_t *cf,rps_command_t *cmd,void *conf){
    rps_http_core_srv_conf_t            *srv;
    rps_str_t                           *values;
    rps_pool_t                          *pool;

    srv = conf;
    pool  = cf -> pool; 
    values = cf -> args -> elts;
    if(srv -> server_name.data != NULL){
        rps_log_error(RPS_ERROR, cf -> log, 0, "sever_name has already been set! FILE:%s,LINE:%lu", cf ->conf_file->file.name.data, cf ->conf_file->line);
        return "server error";
    }
    rps_strcpy(srv -> server_name,values[1],pool); 
    return RPS_CONF_OK;
}
char *rps_set_location_block(rps_conf_t *cf,rps_command_t *cmd,void *conf){
    rps_http_conf_container_t           *srv_container;
    rps_http_core_srv_conf_t            *scf;
    rps_str_t                           *values;
    rps_http_conf_container_t           *new_loc;
    rps_pool_t                          *pool;
    rps_conf_t                           old_cf;
    rps_log_t                           *log;
    rps_uint_t                           i;
    rps_module_t                       **modules;
    rps_cycle_t                         *cycle;
    rps_http_module_t                   *ctx;
    rps_http_core_loc_conf_t            *loc_cf;
    void                               **array_ele;

    values = cf -> args -> elts;
    scf = conf;
    srv_container = cf -> ctx;
    pool = cf -> pool;
    old_cf = *cf;
    log = cf -> log;
    cycle = cf -> cycle;
    modules = cycle -> modules;

    
    array_ele = rps_array_push(&scf -> locations);
    new_loc = rps_palloc(cf -> pool, sizeof(rps_http_conf_container_t));
    if(new_loc == NULL || array_ele == NULL){
        rps_log_error(RPS_LOG_ERR,log,0,"palloc failed in location block");
        return "error";
    }
    *array_ele = new_loc;
    
    new_loc -> loc_conf = rps_palloc(pool, sizeof(void *) * rps_http_modules_n);
    if (new_loc -> loc_conf == NULL){
        rps_log_error(RPS_LOG_ERR,log,0,"palloc failed in location block");
        return "error";
    }
    
    /**
     * 继承srv，main级别配置
     */
    new_loc -> main_conf = srv_container -> main_conf;
    new_loc -> srv_conf = srv_container -> loc_conf;
    
    /**
     * 初始化，loc级别配置
     */
    for (i = 0; modules[i]; i++){
        if( modules[i]-> type == RPS_HTTP_MODULE){
            ctx = modules[i] -> ctx;
            if (ctx != NULL){
                if(ctx -> create_loc_conf != NULL){
                    new_loc -> loc_conf[modules[i] -> ctx_index] = ctx -> create_loc_conf(cf);
                    if (new_loc -> loc_conf[modules[i] -> ctx_index] == NULL){
                        rps_log_error(RPS_LOG_ERR,log,0,"palloc failed in location block");
                        return "error";
                    }
                }
            }
        }
    } 
    rps_log_error (RPS_LOG_DEBUG, cf -> log, 0, "start");
    loc_cf = new_loc -> loc_conf[rps_http_core_module.ctx_index];
    loc_cf -> pattern = (rps_str_t)values[1];

    cf -> ctx = new_loc;
    cf -> cmd_type = RPS_HTTP_LOC_CONF;
    
    rps_log_error(RPS_LOG_DEBUG, log, 0, "prepare to parse loc");
    if(rps_conf_parse(cf) == RPS_ERROR){
        rps_log_error(RPS_LOG_ERR, log, 0, "parse loc failed");
        return "parse error";
    }

    *cf = old_cf;

    return RPS_CONF_OK;
}
void *rps_http_core_create_main_conf(rps_conf_t * cf){
    rps_http_core_main_conf_t               *hcmcf;

    hcmcf = rps_palloc(cf -> pool, sizeof(rps_http_core_main_conf_t));
    if(hcmcf == NULL){
        return NULL;
    }
    if (rps_array_init(&hcmcf-> servers,cf -> pool, 5, sizeof(void*))== RPS_ERROR){
        return NULL;
    }
    hcmcf -> client_max_body_size = RPS_CONF_UNSET_UINT;
    return hcmcf;
}
void *rps_http_core_create_srv_conf(rps_conf_t *cf){
    rps_http_core_srv_conf_t            *srv_conf;

    srv_conf = rps_palloc(cf -> pool,sizeof(rps_http_core_srv_conf_t));
    if (srv_conf == NULL){
        return NULL;
    }
    if(rps_array_init(&srv_conf -> locations, cf -> pool, 5, sizeof(void *)) == RPS_ERROR){
        return NULL;
    }
    srv_conf  -> port = RPS_CONF_UNSET_UINT;
    srv_conf -> server_name = (rps_str_t)rps_null_string;
    return srv_conf;
}
void *rps_http_core_create_loc_conf(rps_conf_t *cf){
    rps_http_core_loc_conf_t            *loc_conf;

    loc_conf = rps_palloc(cf -> pool, sizeof(rps_http_core_loc_conf_t));
    if (loc_conf == NULL){
        return NULL;
    }
    loc_conf -> pattern = (rps_str_t)rps_null_string;
    loc_conf -> exact_match = 0;
    return loc_conf;
}
char *rps_http_core_merge_srv_conf(rps_pool_t *pool, void *parent, void *child){
    rps_http_conf_container_t           *http_main;
    rps_http_conf_container_t           *http_srv;
    rps_http_core_loc_conf_t            *main_loc;
    rps_http_core_srv_conf_t            *main_srv;
    rps_http_core_loc_conf_t            *srv_loc;
    rps_http_core_srv_conf_t            *srv_srv;
    rps_uint_t                           i;

    http_main = parent;
    http_srv = child;
    i = rps_http_core_module.ctx_index;
    main_loc = http_main -> loc_conf[i];
    main_srv = http_main -> srv_conf[i];
    srv_loc = http_srv -> loc_conf[i];
    srv_srv = http_srv -> srv_conf[i];

    
    if (srv_srv -> port == RPS_CONF_UNSET_UINT){
        srv_srv -> port = main_srv -> port;
    }
    if (srv_loc -> pattern.data == NULL){
        srv_loc -> pattern = (rps_str_t)main_loc -> pattern;
    }
    if (srv_loc -> exact_match == 0)
    {
        srv_loc -> exact_match = main_loc -> exact_match;
    }
    return RPS_CONF_OK;
}
char *rps_http_core_merge_loc_conf(rps_pool_t *pool, void *parent, void *child){
    rps_http_conf_container_t           *http_srv;
    rps_http_conf_container_t           *http_loc;
    rps_uint_t                           i;
    rps_http_core_loc_conf_t            *srv_loc;
    rps_http_core_loc_conf_t            *loc_loc;

    i = rps_http_core_module.ctx_index;
    http_srv = parent;
    http_loc = child;

    srv_loc = http_srv -> loc_conf[i];
    loc_loc = http_loc -> loc_conf[i];

    if (loc_loc -> exact_match == 0){
        loc_loc -> exact_match = srv_loc -> exact_match;
    }
    return RPS_CONF_OK;
}