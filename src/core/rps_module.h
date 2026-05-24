#ifndef RPS_MODULE_H_INCLUDED
#define RPS_MODULE_H_INCLUDED

typedef struct rps_module_s    rps_module_t;
#include "rps_config.h"
#include "rps_string.h"
#include "rps_conf_file.h"
#include "rps_cycle.h"

#define RPS_CORE_MODULE     0
#define RPS_EVENT_MODULE    1
#define RPS_HTTP_MODULE     2

#define rps_get_conf(ctx,module) (ctx)[module.index]


struct rps_module_s {
    rps_uint_t              index;       // 模块全局数组索引
    rps_uint_t              ctx_index;   // 模块上下文索引

    rps_str_t               name;

    char                   *version;
    
    void                   *ctx;        // 模块的上下文（存储模块特有的钩子函数,一般就初始化用用咯）
    rps_command_t          *commands;   // 该模块支持的指令数组
    rps_uint_t              type;       //模块类型

    // 模块的生命周期钩子
    //rps_int_t             (*init_master)(rps_log_t *log);
    rps_int_t             (*init_module)(rps_cycle_t *cycle);
    rps_int_t             (*init_process)(rps_cycle_t *cycle);
    void                  (*exit_process)(rps_cycle_t *cycle);
    void                  (*exit_master)(rps_cycle_t *cycle);
};


rps_int_t  rps_preinit_modules(rps_log_t *log);
rps_int_t  rps_cycle_modules(rps_cycle_t *cycle);
rps_int_t  rps_init_modules(rps_cycle_t *cycle);
rps_int_t  rps_count_modules(rps_cycle_t *cycle,rps_uint_t type);


extern rps_module_t     *rps_modules[];
extern rps_uint_t        rps_max_module;
extern char             *rps_module_names[];
extern rps_uint_t        rps_modules_n;
extern rps_uint_t        rps_event_modules_n;
extern rps_uint_t        rps_http_modules_n;

extern rps_module_t rps_core_module;
extern rps_module_t rps_event_module;
extern rps_module_t rps_event_core_module;
extern rps_module_t rps_http_module;
extern rps_module_t rps_http_core_module;
extern rps_module_t rps_http_proxy_module;
extern rps_module_t rps_epoll_module;

#endif