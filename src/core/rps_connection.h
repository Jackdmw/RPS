#ifndef _RPS_CONNECTION_H_INCLUDED_
#define _RPS_CONNECTION_H_INCLUDED_

typedef struct rps_connection_s  rps_connection_t;
typedef struct rps_listening_s  rps_listening_t;

#include "rps_config.h"
#include "rps_string.h"
#include "event/rps_event.h"
#include "rps_log.h"



struct rps_listening_s {
    rps_fd_t            fd;             /* 监听套接字的句柄 */
    struct sockaddr     sockaddr;       /* 监听的本地地址和端口 */
    socklen_t           socklen;        /* 地址长度 */
    
    rps_str_t           addr_text;      /* 格式化后的字符串，如 "0.0.0.0:80" */

    int                 type;           /* SOCK_STREAM (TCP) */

    rps_event_handler_pt handler;       

    void               *servers;        /* 指向该端口下的虚拟主机配置 (如 HTTP 模块) */

    rps_log_t          *log;
    rps_pool_t         *pool;           /* 监听套接字自身的内存池 */

    /* 积压队列长度，即 listen() 的第二个参数 */
    int                 backlog;
    
    /* 标志位 */
    unsigned            open:1;         /* 是否已打开 */
    unsigned            remain:1;       /* 环境变量继承标志 */
};

struct rps_connection_s {

    rps_fd_t            fd;     /** 套接字句柄 */
    rps_event_t        *read;
    rps_event_t        *write;

    rps_cycle_t        *cycle;  /*连接所属cycle*/
    
    struct sockaddr    *sockaddr;   /**远端地址 */
    rps_str_t           addr_text;  /*点分十进制字符串*/

    void               *data;       /*指向具体请求结构体,或者，对于空闲连接对象，指向下一个空闲连接对象*/

    rps_pool_t         *pool;       /*连接专属内存池*/
    rps_listening_t    *listenling; /* 监听端口*/

    unsigned            sent:1;     /*标记是否发送数据*/
    unsigned            close:1;    /*标记是否需要关闭*/

};

/* 1. 监听相关：在大循环开始前调用 */
rps_int_t rps_open_listening_sockets(rps_cycle_t *cycle);
void rps_close_listening_sockets(rps_cycle_t *cycle);

/* 2. 连接生命周期：Accept 时和关闭时调用 */
rps_connection_t *rps_get_connection(rps_cycle_t *cycle, rps_log_t *log,rps_listening_t *listening);
void rps_close_connection(rps_connection_t *c);
void rps_free_connection(rps_connection_t *c);

rps_int_t rps_set_nonblocking(rps_fd_t s);

/* socket I/O 基础操作 */
ssize_t rps_unix_recv(rps_connection_t *c, u_char *buf, size_t size);
ssize_t rps_unix_send(rps_connection_t *c, u_char *buf, size_t size); 
#endif