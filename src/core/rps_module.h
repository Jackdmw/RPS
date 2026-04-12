#ifndef RPS_MODULE_H_INCLUDED
#define RPS_MODULE_H_INCLUDED
#include "rps_core.h"
#include "rps_palloc.h"
#include "rps_array.h"
#include "rps_list.h"


#define RPS_CORE_MODULE     0
#define RPS_EVENT_MODULE    1
#define RPS_HTTP_MODULE     2

typedef struct rps_module_s    rps_module_t;
typedef struct rps_command_s   rps_command_t;





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





#endif