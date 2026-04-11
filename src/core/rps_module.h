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
typedef struct rps_conf_s      rps_conf_t;

#define RPS_CONF_NOARGS             0x00000001
#define RPS_CONF_TAKE1              0x00000002
#define RPS_CONF_TAKE2              0x00000004
#define RPS_CONF_TAKE3              0x00000008
#define RPS_CONF_TAKE4              0x00000010
#define RPS_CONF_TAKE5              0x00000020
#define RPS_CONF_TAKE6              0x00000040
#define RPS_CONF_TAKE7              0x00000080

#define RPS_CONF_MAX_ARGS           8
#define RPS_CONF_TAKE12             (RPS_CONF_TAKE1|RPS_CONF_TAKE2)
#define RPS_CONF_TAKE13             (RPS_CONF_TAKE1|RPS_CONF_TAKE3)

#define RPS_CONF_TAKE23             (RPS_CONF_TAKE2|RPS_CONF_TAKE3)
#define RPS_CONF_TAKE123            (RPS_CONF_TAKE12|RPS_CONF_TAKE3)
#define RPS_CONF_TAKE1234           (RPS_CONF_TAKE12|RPS_CONF_TAKE3|RPS_CONF_TAKE4)

#define RPS_CONF_ARGS_NUMBER        





struct  rps_command_s {
    rps_str_t               name;    // 命令名称
    rps_uint_t              type;    // 指令出现合法层级以及参数个数（使用掩码进行标识）

    char                 *(*set)(rps_conf_t *cf, rps_command_t *cmd,
                                void *conf); // 配置指令处理函数

    rps_uint_t              offset; // 配置结构体中对应成员变量的偏移量
    rps_uint_t              conf;   //标记该配置属于哪个层级（如核心、事件、HTTP）     

};

struct rps_module_s {
    rps_uint_t              index;       // 模块全局数组索引
    rps_uint_t              ctx_index;   // 模块上下文索引

    rps_str_t               name;

    rps_uint_t              version;
    
    void                   *ctx;        // 模块的上下文（存储模块特有的钩子函数）
    rps_command_t          *commands;   // 该模块支持的指令数组
    rps_uint_t              type;       //模块类型

    // 模块的生命周期钩子
    //rps_int_t             (*init_master)(rps_log_t *log);
    rps_int_t             (*init_module)(rps_cycle_t *cycle);
    rps_int_t             (*init_process)(rps_cycle_t *cycle);
    void                  (*exit_process)(rps_cycle_t *cycle);
    void                  (*exit_master)(rps_cycle_t *cycle);
};

struct rps_conf_s {
    rps_str_t           file_name;      // 配置文件名称

    rps_cycle_t        *cycle;          // 当前cycle
    rps_pool_t         *pool;           // 配置使用的内存池

    void               *ctx;            /* 模块上下文 */
    

    void               *conf;      // 配置结构体指针
    rps_module_t       *module;    // 模块指针
};


typedef struct {
    rps_str_t    name;                                    /* 模块名称，例如 "core" */
    void      *(*create_conf)(rps_cycle_t *cycle);        /* 创建配置结构体 */
    char      *(*init_conf)(rps_cycle_t *cycle, void *conf); /* 初始化配置（填默认值） */
}rps_core_module_ctx_t;

#endif