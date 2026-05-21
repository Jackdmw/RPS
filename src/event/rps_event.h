#ifndef _RPS_EVENT_H_INCLUDED_
#define _RPS_EVENT_H_INCLUDED_

typedef struct  rps_event_s   rps_event_t;
typedef void (*rps_event_handler_pt)(rps_event_t *ev);
#include "core/rps_config.h"
#include "core/rps_log.h"
#include "core/rps_rbtree.h"
#include "core/rps_cycle.h"




typedef struct{
    rps_str_t       use;                //事件通知模型，epoll 或者 io_uring
    rps_uint_t      worker_connections; //进程最大连接数
}rps_event_conf_t;

typedef struct{
    void ** event_conf;
}rps_event_container_t;

typedef struct {
    
    void        *(*create_conf)(rps_cycle_t * cycle);
    char        *(*init_conf)(rps_cycle_t   *cycle);
    
}rps_event_module_t;

struct rps_event_s{
    
    void                       *data;           /*指向关联的 rps_connection_t*/
    rps_event_handler_pt        handler;        /* 时间就绪时的回调函数*/

    unsigned                    write:1;        /*1：写事件，0：读事件*/
    unsigned                    active:1;       /*是否注册到epoll/io_uring中*/
    unsigned                    ready:1;        /* 事件是否已就绪*/
    unsigned                    eof:1;          /* 对端是否关闭连接*/
    unsigned                    error:1;        /* 是否发生Socket 错误*/
    unsigned                    timedout:1;     /* 是否超时*/
    unsigned                    timer_set:1;    /* 是否加入红黑树计时器*/

    rps_rbtree_node_t           timer;          /* 红黑树*/

    rps_log_t                   *log;

};


#endif