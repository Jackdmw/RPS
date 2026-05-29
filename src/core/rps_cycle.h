#ifndef RPS_CYCLE_H_INCLUDED
#define RPS_CYCLE_H_INCLUDED

typedef struct rps_cycle_s   rps_cycle_t;
struct rps_event_module_s;  /* 前置声明，避免与 rps_event.h 循环包含 */

#include "rps_list.h"
#include "rps_array.h"
#include  "rps_palloc.h"
#include "rps_log.h"
#include "rps_config.h"
#include "rps_module.h"
#include "rps_connection.h"
#include "event/rps_event.h"


struct rps_cycle_s {
    void                    **conf_ctx; 
    
    rps_pool_t               *pool;             // 周期内存池
    rps_log_t                *log;              // 日志对象
    
    rps_array_t               listening;        // 存储 rps_listening_t
    rps_list_t                open_files;       // 存储已打开文件
    
    rps_module_t            **modules;          // 当前加载的模块
    rps_uint_t                modules_n;        // 模块数量
    
    rps_str_t                 conf_file;        // 配置文件路径 "rps.conf"
    rps_str_t                 prefix;           // 程序安装路径

    rps_str_t                 conf_param;       // -g 传的内联配置
    rps_str_t                 use;              // 事件机制
    rps_uint_t                if_pthread:1;     // 多线程？

    rps_connection_t         *connections;      //worker 进程持有,一个连接池
    rps_connection_t         *free_connection;
    rps_event_t              *reads;
    rps_event_t              *writes;

    struct rps_event_module_s *event_engine;    /* 当前使用的事件驱动引擎（epoll） */
    void                      *event_data;       /* 引擎私有数据（epoll fd / event_list） */
};


rps_cycle_t * rps_init_cycle(rps_cycle_t *old_cycle);


#endif