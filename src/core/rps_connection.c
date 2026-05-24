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
        
    

        bind(listen_array[i].fd, &listen_array[i].sockaddr, listen_array[i].socklen);
        listen(listen_array[i].fd, listen_array[i].backlog);
        rps_log_error(RPS_LOG_INFO,cycle -> log,0,"listen port %s has been create",listen_array[i].addr_text.data);
        listen_array[i].open = 1;
    }
    return RPS_OK;
}
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

        new_conn -> pool = rps_create_pool(1024);
        if(new_conn -> pool == NULL){
            return NULL;
        }

        /* 读写事件指向连接自身 */
        if (new_conn->read) {
            new_conn->read->data = new_conn;
        }
        if (new_conn->write) {
            new_conn->write->data = new_conn;
        }

        return new_conn;
    }
}
void rps_free_connection(rps_connection_t *c,rps_cycle_t *cycle){
    if ( c -> pool != NULL){
        rps_destroy_pool(c->pool);
        c -> pool = NULL;
    }
    c -> data = cycle -> free_connection;
    cycle -> free_connection = c;
    
}
void rps_close_connection(rps_connection_t *c){
    if (c->fd > 0) {
        close(c->fd);
        c->fd = 0;
    }
    if ( c -> pool != NULL){
        rps_destroy_pool(c -> pool);
        c -> pool = NULL;
    }
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