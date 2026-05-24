#include "event/rps_event.h"
#include "core/rps_cycle.h"
#include "core/rps_palloc.h"
#include "core/rps_connection.h"
#include <sys/epoll.h>
#include <errno.h>


static int             epoll_fd      = -1;
static struct epoll_event *event_list = NULL;
static rps_uint_t      nevents       = 512;   /* 每次 epoll_wait 最多取 512 个就绪事件 */


static void *rps_epoll_create_conf(rps_cycle_t *cycle);
static char *rps_epoll_init_conf(rps_cycle_t *cycle);
static rps_int_t rps_epoll_init_process(rps_cycle_t *cycle);
static rps_int_t rps_epoll_add_event(rps_event_t *ev, rps_uint_t event);
static rps_int_t rps_epoll_del_event(rps_event_t *ev, rps_uint_t event);
static rps_int_t rps_epoll_process_events(rps_cycle_t *cycle, rps_msec_t timer);


static rps_command_t rps_epoll_commands[] = {
    rps_null_command
};


static rps_event_module_t rps_epoll_module_ctx = {
    NULL,
    rps_epoll_init_conf,
    rps_epoll_add_event,
    rps_epoll_del_event,
    rps_epoll_process_events,
    rps_epoll_init_process
};

rps_module_t rps_epoll_module = {
    -1,
    -1,
    rps_string("epoll"),
    "1.0.0",
    &rps_epoll_module_ctx,
    rps_epoll_commands,
    RPS_EVENT_MODULE,
    NULL,                       /* init_module   */
    rps_epoll_init_process,     /* init_process  */
    NULL,                       /* exit_process  */
    NULL                        /* exit_master   */
};


static void *
rps_epoll_create_conf(rps_cycle_t *cycle)
{
    rps_event_conf_t *conf;

    conf = rps_palloc(cycle->pool, sizeof(rps_event_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->use               = (rps_str_t)rps_string("epoll");
    conf->worker_connections = RPS_CONF_UNSET_UINT;

    return conf;
}

static char *
rps_epoll_init_conf(rps_cycle_t *cycle)
{
    /* epoll 模块无额外配置校验 */
    return NULL;
}


static rps_int_t
rps_epoll_init_process(rps_cycle_t *cycle)
{
    
    /**
     * 判断是否事件机制使用epoll
     */
    if (cycle -> if_pthread == 1){
        return RPS_OK;
    }
    if (epoll_fd >= 0 ) {
        return RPS_OK;  /* 已初始化 */
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        return RPS_ERROR;
    }

    event_list = rps_palloc(cycle->pool, sizeof(struct epoll_event) * nevents);
    if (event_list == NULL) {
        close(epoll_fd);
        epoll_fd = -1;
        return RPS_ERROR;
    }

    /* 注册为当前进程的事件引擎 */
    cycle->event_engine = &rps_epoll_module_ctx;
    cycle->event_data   = NULL;

    return RPS_OK;
}


static rps_int_t
rps_epoll_add_event(rps_event_t *ev, rps_uint_t event)
{
    int                     op;
    struct epoll_event      ee;
    rps_connection_t       *c;

    c = ev->data;

    memset(&ee, 0, sizeof(ee));

    if (event == RPS_READ_EVENT) {
        ee.events = EPOLLIN;
    } else {
        ee.events = EPOLLOUT;
    }
    ee.data.ptr = ev;

    /**
     * 做了一手封装，自动处理读写事件的监听转换
     */
    if (ev->active) {
        op = EPOLL_CTL_MOD;
    } else {
        op = EPOLL_CTL_ADD;
        ev->active = 1;
    }

    if (epoll_ctl(epoll_fd, op, c->fd, &ee) == -1) {
        return RPS_ERROR;
    }

    return RPS_OK;
}

static rps_int_t
rps_epoll_del_event(rps_event_t *ev, rps_uint_t event)
{
    rps_connection_t       *c;

    c = ev->data;

    if (!ev->active) {
        return RPS_OK;
    }

    ev->active = 0;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL) == -1) {
        return RPS_ERROR;
    }

    return RPS_OK;
}

static rps_int_t
rps_epoll_process_events(rps_cycle_t *cycle, rps_msec_t timer)
{
    int                 events;
    rps_uint_t          i;
    rps_event_t        *ev;
    rps_connection_t   *c;
    int                 timeout;

    timeout = (int)((timer == (rps_msec_t)-1) ? -1 : timer);

    events = epoll_wait(epoll_fd, event_list, (int)nevents, timeout);

    if (events == -1) {
        if (errno == EINTR) {
            return RPS_OK;
        }
        return RPS_ERROR;
    }

    for (i = 0; i < (rps_uint_t)events; i++) {
        ev = (rps_event_t *)event_list[i].data.ptr;
        c  = ev->data;

        /* 连接错误 / 对端关闭 */
        if (event_list[i].events & (EPOLLERR | EPOLLHUP)) {
            ev->eof   = 1;
            ev->error = 1;
        }

        /* 读就绪 */
        if (event_list[i].events & EPOLLIN) {
            ev->ready = 1;
        }

        /* 写就绪 */
        if (event_list[i].events & EPOLLOUT) {
            ev->ready = 1;
            ev -> write = 1;
        }

        if (ev->ready && ev->handler) {
            ev->handler(ev);
        }
    }

    return RPS_OK;
}
