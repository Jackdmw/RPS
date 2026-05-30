#define _GNU_SOURCE     /*打开SO_REUSEPORT 宏*/
#include "rps_connection.h"
#include "rps_cycle.h"
#include "rps_config.h"
#include "http/rps_http_core.h"
#include <fcntl.h>

rps_int_t rps_open_listening_sockets(rps_cycle_t *cycle){
    rps_listening_t            *listen_array;
    rps_uint_t                  n;
    rps_uint_t                  i;
    
    listen_array = cycle -> listening.elts;
    n = cycle -> listening.nelts;


    for(i = 0;i < n; i++){
        listen_array[i].fd = socket(AF_INET,listen_array[i].type,0);
        int opt = 1;
        // 在 bind 之前设置 SO_REUSEADDR以及SO_REUSEPORT
        if (setsockopt(listen_array[i].fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt SO_REUSEADDR failed");
            exit(EXIT_FAILURE);
        }
        if (setsockopt(listen_array[i].fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            perror("setsockopt SO_REUSEPORT failed");
            exit(EXIT_FAILURE);
        }

        bind(listen_array[i].fd, &listen_array[i].sockaddr, listen_array[i].socklen);
        listen(listen_array[i].fd, listen_array[i].backlog);
        rps_log_error(RPS_LOG_INFO,cycle -> log,0,"listen port %s has been create",listen_array[i].addr_text.data);
        listen_array[i].open = 1;
    }
    return RPS_OK;
}
#ifdef _THROWN
/**
 * 这个函数应该废弃，现在的实现中，listensocket是交给connection对象了的
 * 释放交给connection对象去做
 */
void rps_close_listening_sockets(rps_cycle_t *cycle){
    rps_listening_t             *listen_array;
    rps_uint_t                   n;
    rps_uint_t                   i;
    
    listen_array = cycle -> listening.elts;
    n = cycle -> listening.nelts;

    for (i = 0; i < n; i++){
        if (listen_array[i].open == 1){
            close(listen_array[i].fd);
            listen_array[i].open = 0;
        }
    }
}
#endif
/**
 *  从全局周期中拿一个连接对象，连接对象的sockaddr是分配了内存的
 *  read 和 write 事件是按照数组下标的形式，在最开始创建的时候就是一一对应了的
 *  如果要释放，务必一起释放。
 */
rps_connection_t *rps_get_connection(rps_cycle_t *cycle, rps_log_t *log, rps_listening_t *listening){
    rps_connection_t        *new_conn;
    if(cycle -> free_connection == NULL){
        return NULL;
    }
    else {
        new_conn = cycle -> free_connection;
        cycle -> free_connection = cycle -> free_connection -> data;
        
        new_conn->close = 0;
        new_conn ->sent = 0;
        new_conn -> listenling = listening;

        rps_memzero(&new_conn->addr_text, sizeof(rps_str_t));

        new_conn -> pool = rps_create_pool(1024);
        if(new_conn -> pool == NULL){
            return NULL;
        }

        return new_conn;
    }
}
/**
 * 释放连接对象的一切，并且还回cycle
 */
void rps_free_connection(rps_connection_t *c){
    rps_close_connection(c);
    c -> data = c -> cycle -> free_connection;
    c -> cycle -> free_connection = c;
    
}
/**
 * 用于关闭连接，但是，不会还给连接池
 */
void rps_close_connection(rps_connection_t *c){
    if (c->fd > 0) {
        close(c->fd);
        c->fd = 0;
    }

    /* 清除定时器，防止连接回收后残留的 timer 误触发新连接的 handler */
    if (c->read  != NULL) rps_event_del_timer(c->read);
    if (c->write != NULL) rps_event_del_timer(c->write);

    if ( c -> pool != NULL){
        rps_destroy_pool(c -> pool);
        c -> pool = NULL;
    }
    /* 清空远端地址和事件状态，连接回收到空闲链表时不会残留旧数据 */
    rps_memzero(&c->sockaddr, sizeof(struct sockaddr));
    rps_memzero(&c->addr_text, sizeof(rps_str_t));

    if (c->read  != NULL) c->read->active  = 0;
    if (c->write != NULL) c->write->active = 0;
}

rps_int_t
rps_set_nonblocking(rps_fd_t s)
{
    int flags;

    flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) {
        return RPS_ERROR;
    }

    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) {
        return RPS_ERROR;
    }

    return RPS_OK;
}

ssize_t
rps_unix_recv(rps_connection_t *c, u_char *buf, size_t size)
{
    return recv(c->fd, buf, size, 0);
}

ssize_t
rps_unix_send(rps_connection_t *c, u_char *buf, size_t size)
{
    return send(c->fd, buf, size, 0);
}