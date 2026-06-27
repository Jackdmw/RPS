#define _GNU_SOURCE
#include "rps_upstream.h"
#include "http/modules/rps_http_core_module.h"
#include "core/rps_conf_file.h"
#include "core/rps_log.h"
#include "core/rps_cycle.h"
#include "event/rps_event.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>


/* 运行时 — 事件驱动状态机（handler 指针切换推进） */
static void rps_upstream_send_handler(rps_event_t *ev);
static void rps_upstream_read_handler(rps_event_t *ev);
static void rps_upstream_connect(rps_http_request_t *r, rps_upstream_t *u);
static void rps_upstream_cache_idle_handler(rps_event_t *ev);

/* 配置解析 */
static char *rps_upstream_block(rps_conf_t *cf, rps_command_t *cmd, void *conf);
static char *rps_set_upstream_server(rps_conf_t *cf, rps_command_t *cmd, void *conf);


rps_command_t rps_upstream_commands[] = {
    {
        rps_string("upstream"),
        RPS_HTTP_MAIN_CONF | RPS_CONF_TAKE1 | RPS_CONF_BLOCK,
        rps_upstream_block,
        RPS_CONF_BELONG_HTTP_MAIN,
        0,
        NULL
    },
    /* upstream {} 块内部指令 */
    {
        rps_string("server"),
        RPS_HTTP_UPS_CONF | RPS_CONF_TAKE1234,
        rps_set_upstream_server,
        RPS_CONF_BELONG_HTTP_UPS,
        0,
        NULL
    },
    {
        rps_string("keepalive"),
        RPS_HTTP_UPS_CONF | RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_HTTP_UPS,
        offsetof(rps_upstream_conf_t, keepalive),
        NULL
    },
    {
        rps_string("keepalive_timeout"),
        RPS_HTTP_UPS_CONF | RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_HTTP_UPS,
        offsetof(rps_upstream_conf_t, keepalive_timeout),
        NULL
    },
    {
        rps_string("keepalive_requests"),
        RPS_HTTP_UPS_CONF | RPS_CONF_TAKE1,
        rps_conf_set_num_slot,
        RPS_CONF_BELONG_HTTP_UPS,
        offsetof(rps_upstream_conf_t, keepalive_requests),
        NULL
    },
    rps_null_command
};

static rps_http_module_t rps_upstream_module_ctx = {
    NULL,   /* preconfiguration   */
    NULL,   /* postconfiguration  */
    NULL,   /* create_main_conf   */
    NULL,   /* create_srv_conf    */
    NULL,   /* create_loc_conf    */
    NULL,   /* merge_srv_conf     */
    NULL    /* merge_loc_conf     */
};

rps_module_t rps_upstream_module = {
    -1,                             /* index                      */
    -1,                             /* ctx_index                  */
    rps_string("upstream"),          /* name                       */
    "1.0.0",                        /* version                    */
    &rps_upstream_module_ctx,       /* ctx                        */
    rps_upstream_commands,          /* commands                   */
    RPS_HTTP_MODULE,                /* type                       */
    NULL,   /* init_module   */
    NULL,   /* init_process  */
    NULL,   /* exit_process  */
    NULL    /* exit_master   */
};


static char *
rps_upstream_block(rps_conf_t *cf, rps_command_t *cmd, void *conf)
{
    rps_http_core_main_conf_t  *cmcf;
    rps_upstream_conf_t        *ucf;
    rps_conf_t                  old_cf;
    rps_str_t                  *values;
    rps_pool_t                 *pool;
    void                      **array_ele;

    pool   = cf->pool;
    values = cf->args->elts;

    /* upstream 模块没有 create_main_conf，conf 为 NULL。
     * 通过 cf->ctx 获取 http_core 模块的 main_conf。 */
    (void)conf;
    cmcf = ((rps_http_conf_container_t *)cf->ctx)
               ->main_conf[rps_http_core_module.ctx_index];
    if (cmcf == NULL) return "http_core main_conf not found";
    old_cf = *cf;

    ucf = rps_pcalloc(pool, sizeof(rps_upstream_conf_t));
    if (ucf == NULL) {
        return "palloc upstream_conf failed";
    }

    array_ele = rps_array_push(&cmcf->upstreams);
    if (array_ele == NULL) {
        return "push upstreams array failed";
    }
    *array_ele = ucf;

    rps_strcpy(ucf->name, values[1], pool);

    if (rps_array_init(&ucf->peers, pool, 4,
                       sizeof(rps_upstream_peer_t)) == RPS_ERROR) {
        return "init peers array failed";
    }

    if (rps_array_init(&ucf->free_peers, pool, 4,
                       sizeof(rps_upstream_cached_peer_t)) == RPS_ERROR) {
        return "init free_peers array failed";
    }

    ucf->keepalive                 = RPS_CONF_UNSET_UINT;
    ucf->keepalive_timeout          = RPS_CONF_UNSET_MSEC;
    ucf->keepalive_requests         = RPS_CONF_UNSET_UINT;
    ucf->select_peer               = rps_upstream_select_peer_wrr;

    cf->cmd_type = RPS_HTTP_UPS_CONF;
    cf->ctx      = ucf;

    rps_log_error(RPS_LOG_DEBUG, cf->log, 0,
                  "prepare to parse upstream \"%s\" block",
                  ucf->name.data);

    if (rps_conf_parse(cf) == RPS_ERROR) {
        return "parse upstream block failed";
    }

    *cf = old_cf;

    rps_conf_init_uint_value(ucf->keepalive,          0);
    rps_conf_init_msec_value(ucf->keepalive_timeout,  60000);
    rps_conf_init_uint_value(ucf->keepalive_requests, 100);

    return RPS_CONF_OK;
}

/*
 * server <addr> [weight=N] [max_fails=N] [fail_timeout=Ns] [down] [backup];
 */
static char *
rps_set_upstream_server(rps_conf_t *cf, rps_command_t *cmd, void *conf)
{
    rps_upstream_conf_t  *ucf;
    rps_upstream_peer_t  *peer;
    rps_str_t            *values;
    rps_str_t             addr_str;
    u_char               *p;
    rps_uint_t            i;
    rps_int_t             port;

    ucf    = conf;
    values = cf->args->elts;
    addr_str = values[1];

    peer = rps_array_push(&ucf->peers);
    if (peer == NULL) {
        return "push upstream peer failed";
    }

    rps_memzero(peer, sizeof(rps_upstream_peer_t));
    peer->weight       = 1;
    peer->max_fails    = 1;
    peer->fail_timeout = 10000;

    /* 解析 host:port */
    p = addr_str.data;
    port = 0;

    while (p < addr_str.data + addr_str.len && *p != ':') {
        p++;
    }

    {
        rps_str_t tmp_host;
        tmp_host.data = addr_str.data;
        tmp_host.len  = (rps_uint_t)(p - addr_str.data);
        rps_strcpy(peer->host, tmp_host, cf->pool);
    }

    if (p < addr_str.data + addr_str.len && *p == ':') {
        p++;
        while (p < addr_str.data + addr_str.len
               && *p >= '0' && *p <= '9') {
            port = port * 10 + (rps_int_t)(*p - '0');
            p++;
        }
    }
    peer->port = port > 0 ? (rps_uint_t)port : 80;

    /* 可选参数 */
    for (i = 2; i < cf->args->nelts; i++) {
        rps_str_t *arg = &values[i];

        if (arg->len > 7 && memcmp(arg->data, "weight=", 7) == 0) {
            peer->weight = (rps_int_t)rps_atoi(arg->data + 7, arg->len - 7);
        }
        else if (arg->len > 10 && memcmp(arg->data, "max_fails=", 10) == 0) {
            peer->max_fails = rps_atoi(arg->data + 10, arg->len - 10);
        }
        else if (arg->len > 13 && memcmp(arg->data, "fail_timeout=", 13) == 0) {
            rps_uint_t val;
            if (arg->data[arg->len - 1] == 's') {
                val = rps_atoi(arg->data + 13, arg->len - 14);
            } else {
                val = rps_atoi(arg->data + 13, arg->len - 13);
            }
            peer->fail_timeout = (rps_msec_t)(val * 1000);
        }
        else if (rps_strcmp_with_cstr(*arg, "down")) {
            peer->down = 1;
        }
        else if (rps_strcmp_with_cstr(*arg, "backup")) {
            peer->backup = 1;
        }
        else {
            rps_log_error(RPS_LOG_ERR, cf->log, 0,
                "unknown server argument, LINE:%lu, FILE:%s",
                cf->conf_file->line, cf->file_name.data);
        }
    }

    return RPS_CONF_OK;
}


rps_upstream_peer_t *
rps_upstream_select_peer_wrr(rps_upstream_conf_t *ucf)
{
    rps_upstream_peer_t *peers, *best, *backup_best;
    rps_uint_t           i, n;
    rps_int_t            total_weight;
    rps_msec_t           now;

    peers = ucf->peers.elts;
    n     = ucf->peers.nelts;
    if (n == 0) return NULL;

    now         = rps_current_msec();
    best        = NULL;
    backup_best = NULL;
    total_weight = 0;

    for (i = 0; i < n; i++) {
        rps_upstream_peer_t *p = &peers[i];

        if (p->down) continue;

        if (p->fails >= p->max_fails && p->fail_timeout > 0) {
            if (now - p->failed_time < p->fail_timeout) {
                continue;
            }
            p->fails = 0;
        }

        if (p->backup) {
            if (backup_best == NULL
                || p->current_weight > backup_best->current_weight)
            {
                backup_best = p;
            }
            continue;
        }

        p->current_weight += p->effective_weight;
        total_weight      += p->effective_weight;

        if (best == NULL || p->current_weight > best->current_weight) {
            best = p;
        }
    }

    if (best == NULL) {
        best = backup_best;
        if (best == NULL) return NULL;

        best->current_weight += best->effective_weight;
        total_weight = best->effective_weight;
    }

    if (total_weight > 0) {
        best->current_weight -= total_weight;
    }

    return best;
}

/**
 * 由http core postconfiguration 调用
 */
void
rps_upstream_init_peers(rps_upstream_conf_t *ucf)
{
    rps_upstream_peer_t *peers;
    rps_uint_t           i;

    peers = ucf->peers.elts;
    for (i = 0; i < ucf->peers.nelts; i++) {
        peers[i].effective_weight = peers[i].weight;
        peers[i].current_weight   = 0;
        peers[i].fails            = 0;
        peers[i].failed_time      = 0;
    }
}


/*
 * 获取一个 upstream  connection 对象。
 * 没有做连接！！！
 * 优先从 cycle->free_upstream_connections 空闲链表取（复用已分配的内存），
 * 链表空时才从 cycle->pool 新分配。所有 upstream 块共享同一个连接对象池。
 */
static rps_connection_t *
rps_upstream_new_peer_conn(rps_cycle_t *cycle, rps_log_t *log,
                            rps_upstream_conf_t *ucf)
{
    rps_connection_t *c;

    (void)ucf;

    /* 先从 cycle 级空闲链表取（thread 模式下加锁） */
    if (cycle->if_pthread) pthread_mutex_lock(&cycle->upstream_conn_mutex);

    if (cycle->free_upstream_connections != NULL) {
        c = cycle->free_upstream_connections;
        cycle->free_upstream_connections = c->data;

        if (cycle->if_pthread) pthread_mutex_unlock(&cycle->upstream_conn_mutex);

        /* 重置事件状态 */
        rps_memzero(c->read,  sizeof(rps_event_t));
        rps_memzero(c->write, sizeof(rps_event_t));

        c->read->data  = c; c->read->connection = c;
        c->read->connection = c;
        c->read->log   = log;
        c->read->write = 0;

        c->write->data  = c;
        c->write->connection = c;
        c->write->log   = log;
        c->write->write = 1;

        c->data = NULL;
        c->upstream_requests = 0;
        return c;
    }

    if (cycle->if_pthread) pthread_mutex_unlock(&cycle->upstream_conn_mutex);

    /* 空闲链表空，新分配 */
    {
        rps_event_t *read, *write;

        c     = rps_pcalloc(cycle->pool, sizeof(rps_connection_t));
        read  = rps_pcalloc(cycle->pool, sizeof(rps_event_t));
        write = rps_pcalloc(cycle->pool, sizeof(rps_event_t));

        if (c == NULL || read == NULL || write == NULL) return NULL;

        c->read  = read;
        c->write = write;
        c->cycle = cycle;

        read->data  = c; read->connection = c;
        read->log   = log;
        read->write = 0;

        write->data  = c; write->connection = c;
        write->log   = log;
        write->write = 1;

        return c;
    }
}

/*
 * 关闭后端连接的 fd、清理 epoll 事件和定时器，
 * 然后将connection对象归还到 cycle 级空闲链表供后续复用。
 */
void
rps_upstream_close_peer_conn(rps_connection_t *c)
{
    if (c == NULL) return;

    if (c->fd > 0) {
        if (c->cycle != NULL && c->cycle->event_engine != NULL) {
            if (c->read  != NULL && c->read->active)
                c->cycle->event_engine->del_event(c->read,  RPS_READ_EVENT);
            if (c->write != NULL && c->write->active)
                c->cycle->event_engine->del_event(c->write, RPS_WRITE_EVENT);
        }

        if (c->read  != NULL) { rps_event_del_timer(c->read);  c->read->active  = 0; }
        if (c->write != NULL) { rps_event_del_timer(c->write); c->write->active = 0; }

        close(c->fd);
        c->fd = 0;
    }

    /* 归还到 cycle 级空闲链表（thread 模式下加锁） */
    if (c->cycle->if_pthread) pthread_mutex_lock(&c->cycle->upstream_conn_mutex);
    c->data = c->cycle->free_upstream_connections;
    c->cycle->free_upstream_connections = c;
    if (c->cycle->if_pthread) pthread_mutex_unlock(&c->cycle->upstream_conn_mutex);
}

/*
 * 发起非阻塞 connect 到 u->peer_addr。
 * 在缓存没命中后执行，这个时候peer host 和port 已经获取到了
 * 使用独立分配的后端连接。
 */
static void rps_upstream_connect(rps_http_request_t *r, rps_upstream_t *u)
{
    int              fd;
    struct addrinfo  hints, *res, *rp;
    char             host_buf[256], port_buf[8];
    size_t           host_len;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    host_len = u->peer_addr.host.len;
    if (host_len >= sizeof(host_buf)) host_len = sizeof(host_buf) - 1;
    memcpy(host_buf, u->peer_addr.host.data, host_len);
    host_buf[host_len] = '\0';
    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)u->peer_addr.port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host_buf, port_buf, &hints, &res) != 0) {
        close(fd);
        return;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        if (errno == EINPROGRESS) break;
    }
    freeaddrinfo(res);

    if (rp == NULL) { close(fd); return; }

    u->peer = rps_upstream_new_peer_conn(r->cycle, r->cycle->log,
                                          u->upstream_conf);
    if (u->peer == NULL) { close(fd); return; }
    u->peer->fd = fd;
}

/**
 * 创建upstream对象
 */
rps_upstream_t *
rps_upstream_create(rps_http_request_t *r)
{
    rps_upstream_t  *u;

    u = rps_pcalloc(r->pool, sizeof(rps_upstream_t));
    if (u == NULL) return NULL;

    u->connect_timeout  = 60000;
    u->send_timeout     = 60000;
    u->read_timeout     = 60000;
    u -> content_length_n = 0;
    u->response_buf = rps_buf_create(r->pool, 16384);
    if (u->response_buf == NULL) return NULL;

    return u;
}

/**
 * 进行upstream 流程
 * 进入前需要挂载header解析钩子，request 构造钩子
 * 还需要初始化对端的host port，如果是属于upstream 块中定义的，可省略
 */
void
rps_upstream_init(rps_http_request_t *r, rps_upstream_t *u)
{
    /* 如果配置了 upstream 块，通过负载均衡选择 peer */
    if (u->upstream_conf != NULL) {
        rps_upstream_peer_t *peer;

        peer = u->upstream_conf->select_peer(u->upstream_conf);
        if (peer == NULL) {
            rps_upstream_finalize(r, RPS_ERROR);
            return;
        }
        u->peer_addr.host = peer->host;
        u->peer_addr.port = peer->port;
        rps_log_error(RPS_LOG_INFO, r->cycle -> log, 0, "reset upstream link addr for host is matched with upstream block");
    }

    /* 协议回调：构造发往后端的请求 */
    if (u->create_request) {
        if (u->create_request(r, u) != RPS_OK) {
            rps_log_error(RPS_LOG_ERR, r->log, 0, "upstream: create_request failed");
            rps_upstream_finalize(r, RPS_ERROR);
            return;
        }
    }

    /* 获取后端连接（优先 keepalive 缓存） */
    rps_upstream_get_peer(r, u);
    if (u->peer == NULL) {
        rps_log_error(RPS_LOG_ERR, r->log, 0, "upstream: get_peer returned NULL");
        rps_upstream_finalize(r, RPS_ERROR);
        return;
    }

    /*
     * 注册事件 — 隐式状态机：
     *   send_handler 首次触发时检查 connect 结果，然后发送请求。
     *   全部发送后 del write、add read，read_handler 接管后续。
     *
     * 保持 ev->data = 连接对象（rps_epoll_add_event 需要它获取 fd），
     * 请求对象通过 c->data 传递（与客户端连接保持一致的模式）。
     */
    u->peer->write->handler = rps_upstream_send_handler;
    u->peer->read->handler  = rps_upstream_read_handler;
    u->peer->data           = r; 
    u->peer->read->data = r; 
    u->peer->write->data = r;

    if (r->cycle->event_engine->add_event(u->peer->write,
                                          RPS_WRITE_EVENT) != RPS_OK) {
        rps_log_error(RPS_LOG_ERR, r->log, 0,
                      "upstream: add_event(write) failed fd=%d", u->peer->fd);
        rps_upstream_finalize(r, RPS_ERROR);
        return;
    }

    rps_event_add_timer(u->peer->write, u->connect_timeout);
}

/*
 * 写事件回调 — connect 完成检查 + 发送请求。
 *
 * 首次触发（request_sent == 0）: 检查 SO_ERROR 确认 connect 成功。
 * 后续触发: 继续发送未完成的请求数据。
 * 全部发送完毕: del write、add read，由 read_handler 接管。
 */
static void
rps_upstream_send_handler(rps_event_t *ev)
{
    rps_connection_t   *c = ev->connection;
    rps_http_request_t *r;
    rps_upstream_t     *u;

    if (c == NULL) return;
    r = ev->data;
    if (r == NULL) return;
    u = r->upstream;
    if (u == NULL) return;

    if (ev->timedout) {
        rps_upstream_finalize(r, RPS_ERROR);
        return;
    }

    /* 首次触发：connect 完成，检查 SO_ERROR */
    if (u->request_sent == 0 && ev->timer_set) {
        int       sock_err;
        socklen_t len = sizeof(sock_err);

        rps_event_del_timer(ev);

        if (getsockopt(u->peer->fd, SOL_SOCKET, SO_ERROR,
                       &sock_err, &len) < 0 || sock_err != 0) {
            rps_upstream_finalize(r, RPS_ERROR);
            return;
        }
        rps_log_error(RPS_LOG_DEBUG, ev -> log, 0, "connect to upstream successfully");
        /* connect 成功，切换到发送超时 */
        rps_event_add_timer(ev, u->send_timeout);
    }
    /* 发送请求 */
    {
        ssize_t n = rps_unix_send(u->peer,
                         u->request_bufs->pos + u->request_sent,
                         (size_t)(u->request_bufs->last - u->request_bufs->pos)
                             - u->request_sent);

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            rps_upstream_finalize(r, RPS_ERROR);
            return;
        }

        u->request_sent += (size_t)n;

        if (u->request_sent <
            (size_t)(u->request_bufs->last - u->request_bufs->pos)) {
            return;  /* 还有数据，保持写事件等下次可写 */
        }
    }

    /* 请求发送完毕，切换到读 */
    rps_event_del_timer(ev);
    r->cycle->event_engine->del_event(u->peer->write, RPS_WRITE_EVENT);

    if (r->cycle->event_engine->add_event(u->peer->read, RPS_READ_EVENT) != RPS_OK) {
        rps_upstream_finalize(r, RPS_ERROR);
        return;
    }
    rps_event_add_timer(u->peer->read, u->read_timeout);
}

/*
 * 读事件回调 — 状态机驱动: READ_HEADER → READ_BODY。
 *
 * READ_HEADER: process_response 解析头、发送头
 * READ_BODY:   收 body → write_filter 推送 → CL 完成判定
 */
static void
rps_upstream_read_handler(rps_event_t *ev)
{
    rps_connection_t   *c = ev->connection;
    rps_http_request_t *r;
    rps_upstream_t     *u;
    ssize_t             n;

    if (c == NULL) return;
    r = ev->data;
    if (r == NULL) return;
    u = r->upstream;
    if (u == NULL) return;

    if (ev->timedout) {
        rps_log_error(RPS_LOG_EMERG, ev -> log, 0, "bizserver down ?");
        rps_upstream_finalize(r, RPS_ERROR);
        return;
    }

    n = rps_unix_recv(u->peer, u->response_buf->last,
                      (size_t)(u->response_buf->end - u->response_buf->last));
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        rps_upstream_finalize(r, RPS_ERROR);
        return;
    }

    if (n == 0) {
        if (u->keepalive) u->keepalive = 0;
        rps_log_error(RPS_LOG_INFO, ev -> log, 0, "upstream close the connection");
        rps_upstream_finalize(r, RPS_OK);
        return;
    }
    u->response_buf->last += n;

    /* ── READ_HEADER：解析后端响应头，发送给客户端 ── */
    if (u->read_state == RPS_UPSTREAM_READ_HEADER) {
        rps_int_t rc = u->process_response(r, u);
        if (rc == RPS_AGAIN) return;
        if (rc != RPS_OK) {
            rps_upstream_finalize(r, RPS_ERROR);
            return;
        }
        u->read_state = RPS_UPSTREAM_READ_BODY;
        /*
         * process_response 已设置 body_received 和 buf->pos（指向 body 起始）。
         * 不跳出去等下次事件，直接进入 READ_BODY 把 buffer 里已有的 body 发出去。
         */
    } else {
        /* 后续读：累加新收到的 body 字节数 */
        u->body_received += (size_t)n;
    }

    /* ── READ_BODY：推送 body 数据，判定完成 ── */
    if (u->content_length_n > 0) {
        rps_int_t rc;

        rc = rps_http_write_filter(r);
        if (rc == RPS_AGAIN) {
            if (u->peer && u->peer->read && u->peer->read->active)
                r->cycle->event_engine->del_event(u->peer->read, RPS_READ_EVENT);
            r->connection->write->handler = u->write_continue;
            return;
        }
        if (rc != RPS_OK) { rps_upstream_finalize(r, RPS_ERROR); return; }

        /* 发完复位 buf */
        if (u->response_buf->pos == u->response_buf->last) {
            u->response_buf->pos  = u->response_buf->start;
            u->response_buf->last = u->response_buf->start;
        }

        /* CL 已知且收完 → 删后端读，结束 */
        if (u->body_received >= u->content_length_n) {
            if (u->peer && u->peer->read && u->peer->read->active)
                r->cycle->event_engine->del_event(u->peer->read, RPS_READ_EVENT);
            else
                rps_event_del_timer(u->peer->read);
            rps_upstream_finalize(r, RPS_OK);
        }
    } else {
        /* 无 CL：header 已发，无 body，直接完成 */
        if (u->peer && u->peer->read && u->peer->read->active)
            r->cycle->event_engine->del_event(u->peer->read, RPS_READ_EVENT);
        else
            rps_event_del_timer(u->peer->read);
        rps_upstream_finalize(r, RPS_OK);
    }
}

/*
 * 结束 upstream 处理。
 *
 * 职责:
 *   1. 错误时发送 502 响应
 *   2. 归还或关闭后端连接（通过 rps_upstream_free_peer）
 *   3. finalize HTTP 请求 + complete 客户端连接
 *
 * 由 upstream 内部事件回调（超时 / 错误 / EOF）调用。
 * 调用后 r 已销毁，调用者不可再访问。
 */
void
rps_upstream_finalize(rps_http_request_t *r, rps_int_t rc)
{
    rps_upstream_t   *u;
    rps_connection_t *c;
    rps_int_t         send_rc = RPS_OK;

    u = r->upstream;
    c = r->connection;
    if (u == NULL) {
        rps_http_finalize_request(r, rc);
        rps_http_complete_request(c);
        return;
    }

    c = r->connection;

    /*
     * 上游失败：向客户端发送 502 错误响应。
     * 必须检查返回值：若 EAGAIN，写事件已注册 (data = r)，
     * 此时不可销毁 r，交由 write_filter_continue 善后。
     */
    if (rc != RPS_OK && c && c->fd > 0) {
        r->headers_out.status.value = (rps_str_t)rps_string("502 Bad Gateway");
        send_rc = rps_http_send_response(r);
    }

    /* 归还或关闭后端连接 */
    if (u->peer) {
        if (u->peer->fd > 0) {
            rps_event_del_timer(u->peer->write);
            rps_event_del_timer(u->peer->read);

            if (u->peer->write->active)
                r->cycle->event_engine->del_event(u->peer->write,
                                                  RPS_WRITE_EVENT);
            if (u->peer->read->active)
                r->cycle->event_engine->del_event(u->peer->read,
                                                  RPS_READ_EVENT);
        }

        rps_upstream_free_peer(u);
    }

    r->upstream = NULL;

    /*
     * send_rc == RPS_AGAIN 表示 502 响应尚未发送完毕，
     * write_filter_continue 回调会在写就绪后 finalize r。
     * 此时上游已完全清理，r 留给写事件回调销毁。
     */
    if (send_rc == RPS_AGAIN) {
        return;
    }

    rps_http_finalize_request(r, rc);
    rps_http_complete_request(c);
}



/*
 * keepalive 缓存
 * 空闲连接超时回调：从缓存中移除并释放连接。
 */
static void
rps_upstream_cache_idle_handler(rps_event_t *ev)
{
    rps_connection_t       *peer = ev->connection;
    rps_upstream_conf_t    *ucf;
    rps_upstream_cached_peer_t *cached;
    rps_uint_t              i;

    if (peer == NULL) return;

    /*
     * 遍历 upstream 缓存栈找到该连接并移除。
     * peer->data 存储了指向 upstream_conf 的指针（在 free_peer 中设置）。
     */
    ucf = peer->data;
    if (ucf == NULL) {
        rps_upstream_close_peer_conn(peer);
        return;
    }

    cached = ucf->free_peers.elts;
    for (i = 0; i < ucf->free_peers.nelts; i++) {
        if (cached[i].connection == peer) {
            /* 移除：将最后一个元素移到当前位置，缩减数组 */
            if (i < ucf->free_peers.nelts - 1) {
                cached[i] = cached[ucf->free_peers.nelts - 1];
            }
            ucf->free_peers.nelts--;
            break;
        }
    }

    rps_event_del_timer(ev);
    rps_upstream_close_peer_conn(peer);
}

/*
 * 获取后端连接：先查 keepalive 缓存，未命中则新建 + connect。
 */
rps_connection_t *
rps_upstream_get_peer(rps_http_request_t *r, rps_upstream_t *u)
{
    rps_upstream_conf_t        *ucf = u->upstream_conf;
    rps_upstream_cached_peer_t *cached;

    /* 如有 upstream 配置且缓存非空，LIFO 取栈顶 */
    if (ucf != NULL && ucf->free_peers.nelts > 0) {
        cached = ucf->free_peers.elts;
        cached += ucf->free_peers.nelts - 1;

        rps_connection_t *peer = cached->connection;

        /* 检查 keepalive_requests 限制：超限则关闭缓存连接并走新建路径 */
        peer->upstream_requests++;
        if (peer->upstream_requests > ucf->keepalive_requests) {
            rps_event_del_timer(peer->read);
            peer->read->timedout = 0;
            ucf->free_peers.nelts--;
            rps_upstream_close_peer_conn(peer);
            goto new_peer;
        }

        /* 移除空闲定时器 */
        rps_event_del_timer(peer->read);
        peer->read->timedout = 0;

        /* 弹出缓存栈 */
        ucf->free_peers.nelts--;

        u->peer = peer;
        return peer;
    }

new_peer:
    /* 缓存未命中或超限：新建连接 */
    rps_upstream_connect(r, u);
    return u->peer;
}

/*
 * 归还后端连接：可复用则放入缓存并设空闲定时器，否则关闭。
 */
void
rps_upstream_free_peer(rps_upstream_t *u)
{
    rps_upstream_conf_t        *ucf = u->upstream_conf;
    rps_upstream_cached_peer_t *cached;
    rps_connection_t           *peer = u->peer;

    u->peer = NULL;

    if (peer == NULL) return;

    /* 判断是否可缓存：需同时满足配置允许 + 后端同意 + 缓存未满 */
    if (ucf == NULL || ucf->keepalive == 0
        || u->keepalive == 0
        || ucf->free_peers.nelts >= ucf->keepalive)
    {
        /* 不缓存：直接释放 */
        rps_upstream_close_peer_conn(peer);
        return;
    }

    /* 放入缓存栈顶 */
    cached = rps_array_push(&ucf->free_peers);
    if (cached == NULL) {
        rps_upstream_close_peer_conn(peer);
        return;
    }

    cached->connection = peer;
    cached->idle_since = rps_current_msec();

    /*
     * 设置空闲超时定时器。
     * 复用 peer->read 事件（此时已无活跃 IO），
     * peer->data 暂存 upstream_conf 指针以便超时回调查找。
     */
    peer->data = ucf;
    peer->read->handler = rps_upstream_cache_idle_handler;

    rps_event_add_timer(peer->read, ucf->keepalive_timeout);
}
