#include "rps_connection.h"
#include "rps_cycle.h"
#include "rps_config.h"
#include "http/rps_http_core.h"

rps_int_t rps_open_listening_sockets(rps_cycle_t *cycle){
    rps_listening_t            *listen_array;
    rps_uint_t                  n;
    struct sockaddr            *sockaddr;
    rps_uint_t                  i;
    rps_http_conf_container_t  *server;
    rps_int_t                   c;
    rps_event_conf_t           *conf;
    
    listen_array = cycle -> listening.elts;
    n = cycle -> listening.nelts;


    for(i = 0;i < n; i++){
        listen_array[i].fd = socket(AF_INET,listen_array[i].type,0);
        
    

        bind(listen_array[i].fd,listen_array[i].sockaddr,listen_array[i].socklen);
        
        server = listen_array[i].servers;
        /**
         * 这里后面把http模块完善后，应该从server中拿虚拟主机处理的连接数，然后给端口监听。
         */
        
        listen(listen_array[i].fd,listen_array[i].backlog);
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
        return new_conn;
    }
}
void rps_free_connection(rps_connection_t *c,rps_cycle_t *cycle){
    rps_destroy_pool(c->pool);
    c -> data = cycle -> free_connection;
    cycle -> free_connection = c;
    
}
void rps_close_connection(rps_connection_t *c){
    close (c->fd);
    c -> fd = 0;

}