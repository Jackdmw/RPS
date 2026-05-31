#ifndef _RPS_UPSTREAM_H_INCLUDED_
#define _RPS_UPSTREAM_H_INCLUDED_

typedef struct rps_upstream_s       rps_upstream_t;
typedef struct rps_upstream_conf_s  rps_upstream_conf_t;
typedef struct rps_upstream_peer_s  rps_upstream_peer_t;

#include "core/rps_config.h"
#include "core/rps_string.h"
#include "core/rps_array.h"
#include "core/rps_connection.h"
#include "core/rps_palloc.h"
#include "core/rps_module.h"
#include "http/rps_http_core.h"

/*
 * 通用 upstream 协议接口。
 * 任何协议模块（proxy / fastcgi / uwsgi）实现此回调集即可复用
 * upstream 框架（连接管理 + 负载均衡 + keepalive）。
 */
typedef struct {
    rps_int_t (*create_request)(rps_http_request_t *r, rps_upstream_t *u);
    rps_int_t (*process_header)(rps_http_request_t *r, rps_upstream_t *u);
    rps_int_t (*init_upstream)(rps_upstream_t *u, rps_upstream_conf_t *ucf);
    void      (*finalize)(rps_http_request_t *r, rps_int_t rc);
} rps_upstream_proto_t;

/*
 * 对端地址
 */
typedef struct {
    struct sockaddr     sockaddr;
    socklen_t           socklen;
    rps_str_t           host;
    rps_uint_t          port;
} rps_upstream_addr_t;

/*
 * 单个后端节点（peer）—— 配置时填充
 */
struct rps_upstream_peer_s {
    rps_str_t       host;
    rps_uint_t      port;

    rps_int_t       weight;             /* 权重，默认 1 */
    rps_uint_t      max_fails;          /* 最大失败次数，默认 1 */
    rps_msec_t      fail_timeout;       /* 失败冷却时间（毫秒），默认 10000 */

    /* WRR 运行时状态 */
    rps_int_t       current_weight;
    rps_int_t       effective_weight;
    rps_uint_t      fails;
    rps_msec_t      failed_time;

    unsigned        down:1;
    unsigned        backup:1;
};

/*
 * 缓存的后端连接（keepalive 空闲连接栈中的一项）
 */
typedef struct {
    rps_connection_t    *connection;        /* 空闲的后端连接 */
    rps_uint_t           requests;          /* 已服务的请求数 */
    rps_msec_t           idle_since;        /* 放入缓存的时间戳 */
} rps_upstream_cached_peer_t;

/*
 * upstream 组配置 — 对应一个 upstream {} 块
 */
struct rps_upstream_conf_s {
    rps_str_t       name;
    rps_array_t     peers;              /* rps_upstream_peer_t[] */

    rps_uint_t      keepalive;
    rps_msec_t      keepalive_timeout;
    rps_uint_t      keepalive_requests;

    rps_upstream_peer_t *(*select_peer)(rps_upstream_conf_t *ucf);

    /* keepalive 空闲连接缓存（LIFO 栈） */
    rps_array_t     free_peers;         /* rps_upstream_cached_peer_t[] */
};

/*
 * 运行时 upstream 上下文（per-request，从 r->pool 分配）
 *
 * 状态机通过事件 handler 指针切换推进，不维护显式 state 变量：
 *
 *   init:
 *     peer->write->handler = rps_upstream_send_handler
 *     peer->read->handler  = rps_upstream_read_handler
 *     add_event(write) → 写就绪后 send_handler 检查 connect 结果并发送请求
 *
 *   send 完成:
 *     del_event(write), add_event(read)
 *     → 读就绪后 read_handler 收响应头 → 收响应体 → 转发客户端
 *
 *   对端关闭 / 出错 / 超时:
 *     → rps_upstream_finalize(r, rc)
 */
struct rps_upstream_s {
    rps_connection_t            *peer;              /* 后端连接 */
    rps_upstream_addr_t          peer_addr;         /* 对端地址 */
    rps_upstream_conf_t         *upstream_conf;     /* upstream {} 块配置，可为 NULL */

    /* 缓冲区 */
    rps_buf_t                   *request_bufs;      /* 待发送请求 */
    size_t                       request_sent;      /* 已发送字节数 */
    rps_buf_t                   *response_buf;      /* 接收缓冲区 */

    /* 超时（毫秒） */
    rps_msec_t                   connect_timeout;
    rps_msec_t                   send_timeout;
    rps_msec_t                   read_timeout;

    /* 协议回调 */
    rps_upstream_proto_t        *proto;             /* 协议模块回调集 */
    rps_int_t (*create_request)(rps_http_request_t *r, rps_upstream_t *u);
    rps_int_t (*process_header)(rps_http_request_t *r, rps_upstream_t *u);

    /* 标记 */
    unsigned                    header_complete:1;   /* 响应头是否已解析完毕 */
};

/* 创建 upstream 上下文（从 r->pool 分配） */
rps_upstream_t *rps_upstream_create(rps_http_request_t *r);

/*
 * 启动 upstream：选择 peer → 构造请求 → 发起连接 → 注册事件。
 * 失败时内部调用 rps_upstream_finalize。
 */
void rps_upstream_init(rps_http_request_t *r, rps_upstream_t *u);

/*
 * 结束 upstream 处理：清理后端连接，发送错误响应（502），
 * 最终化 HTTP 请求并决定客户端连接命运。
 * 由 upstream 内部事件回调（超时/错误/EOF）调用。
 */
void rps_upstream_finalize(rps_http_request_t *r, rps_int_t rc);


/* 加权轮询选择 peer */
rps_upstream_peer_t *rps_upstream_select_peer_wrr(rps_upstream_conf_t *ucf);

/* 初始化 peer 的 WRR 运行时状态（postconfiguration 阶段调用） */
void rps_upstream_init_peers(rps_upstream_conf_t *ucf);

/* ── keepalive 缓存 API ── */

/*
 * 获取后端连接：先查 keepalive 缓存，未命中则新建 + connect。
 * 返回的连接已 connect 完成（非阻塞），调用者直接 add_event(write) 即可。
 */
rps_connection_t *rps_upstream_get_peer(rps_http_request_t *r, rps_upstream_t *u);

/*
 * 归还后端连接：可复用则放入缓存并设空闲定时器，否则关闭。
 */
void rps_upstream_free_peer(rps_upstream_t *u);

extern rps_module_t rps_upstream_module;

#endif /* _RPS_UPSTREAM_H_INCLUDED_ */
