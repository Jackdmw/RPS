#include "rps_core_module.h"
#include  "rps_core.h"
#include "rps_cycle.h"
#include "rps_conf_file.h"

char * rps_set_worker_processes(rps_conf_t *cf,rps_command_t *cmd,void * conf);
char * rps_set_daemon(rps_conf_t *cf,rps_command_t *cmd,void *conf);
char * rps_set_pid(rps_conf_t *cf,rps_command_t *cmd,void *conf);

void * rps_core_create_conf(rps_cycle_t *cycle);
char * rps_core_init_conf(rps_cycle_t * cycle);

static rps_command_t rps_core_commands[] = {
    {   rps_string("worker_processes"),
        RPS_MAIN_CONF|RPS_CONF_TAKE1,
        rps_set_worker_processes,
        RPS_CONF_BELONG_CORE,offsetof(rps_core_conf_t,worker_processes),NULL
    },
    {
        rps_string("daemon"),
        RPS_MAIN_CONF|RPS_CONF_TAKE1,
        rps_conf_set_flag_slot,
        RPS_CONF_BELONG_CORE,offsetof(rps_core_conf_t,daemon),NULL
    },
    {
        rps_string("pid"),
        RPS_MAIN_CONF|RPS_CONF_TAKE1,
        rps_conf_set_str_slot,
        RPS_CONF_BELONG_CORE,offsetof(rps_core_conf_t,pid),NULL
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
    NULL,
};

void *rps_core_create_conf(rps_cycle_t * cycle){
    rps_core_conf_t  *ccf;
    ccf = rps_pcalloc(cycle->pool,sizeof(rps_core_conf_t));
    if(ccf == NULL) return NULL;

    ccf->worker_processes  =  RPS_CONF_UNSET_UINT;
    ccf->daemon = RPS_CONF_UNSET_UINT;
    ccf->pid = (rps_str_t)rps_string("run_pid.conf");

    return ccf;
}

char *rps_set_worker_processes(rps_conf_t *cf,rps_command_t *cmd,void * conf){

    rps_uint_t n;

    rps_core_conf_t *ccf = (rps_core_conf_t *)conf;
    rps_str_t * values = cf -> args -> elts;
    
    // 已经被初始化了
    if(ccf->worker_processes != RPS_CONF_UNSET_UINT){
        rps_log_error(RPS_LOG_ALERT,cf->log,0,"\"worker_process\" has already been duplicated ");
        return "is duplicate";
    }
    if(rps_strcmp_with_cstr( values[1], "auto")){
        ccf -> worker_processes = 1 ;
        return RPS_CONF_OK;
    }
    n = rps_atoi(values[1].data,values[1].len);
    if (n == RPS_ERROR){
        rps_log_error(RPS_LOG_ERR,cf->log,0,"\"worker_process\" error, the arg should be a num!");
        return "arg error";
    }
    ccf -> worker_processes = n;
    return RPS_CONF_OK;
    
}

char *rps_set_daemon(rps_conf_t *cf,rps_command_t *cmd,void *conf)
{
    rps_core_conf_t  *ccf = (rps_core_conf_t *)conf;
    rps_str_t * values = cf ->args ->elts;

    if(ccf->daemon != RPS_CONF_UNSET_UINT){
        rps_log_error(RPS_LOG_ALERT,cf->log,0,"\"daemon\" has already been duplicated ");
        return "is duplicate";
    }
    if(rps_strcmp_with_cstr(values[1],"on")){
        ccf->daemon = 1;
        return RPS_CONF_OK;
    }
    else if (rps_strcmp_with_cstr(values[1],"off")){
        ccf->daemon = 0;
        return RPS_CONF_OK;
    }
    else {
        rps_log_error(RPS_LOG_ERR,cf->log,0,"\"daemon\":arg could only be in \"on\" and \"off\" !");
        return "arg error";
    }
}

char *
rps_set_pid(rps_conf_t *cf,rps_command_t *cmd,void *conf)
{
    rps_core_conf_t *ccf = (rps_core_conf_t*)conf;
    rps_str_t *values =cf ->args->elts;
    rps_pool_t  *pool = cf->pool;
    rps_str_t   *new_str;

    if(!rps_strcmp_with_cstr(ccf->pid,"default.pid.txt")){
        rps_log_error(RPS_LOG_ALERT,cf->log,0,"\"pid\" has already been duplicated!");
        return "is duplicate";
    }
    // 分配内存，避免buf被清空后导致出现段错误
    rps_strcpy(ccf->pid,values[1],pool);

    if(ccf->pid.data == NULL)
    {
        rps_log_error(RPS_LOG_ERR,cf->log,0,"memory malloc failed");
        return "memory malloc failed";
    }
    return RPS_CONF_OK;
}

char * rps_core_init_conf(rps_cycle_t * cycle){
    rps_core_conf_t     *ccf;

    ccf = rps_get_conf(cycle -> conf_ctx,rps_core_module);

    rps_conf_init_uint_value(ccf -> daemon,0);
    rps_conf_init_value(ccf -> worker_processes,1);
    return NULL;
}