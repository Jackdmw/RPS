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
 * 对端地址（运行时解析后的结果）
 */
typedef struct {
    struct sockaddr     sockaddr;       /* 解析后的 socket 地址 */
    socklen_t           socklen;        /* 地址长度 */
    rps_str_t           host;           /* 目标主机名/IP */
    rps_uint_t          port;           /* 目标端口 */
} rps_upstream_addr_t;

/*
 * 单个后端节点（peer）—— upstream {} 块内的 server 指令解析产物
 *
 * 全部字段在配置解析阶段填充，WRR 运行时状态在 rps_upstream_init_peers 初始化。
 */
struct rps_upstream_peer_s {
    /* ── 配置字段（server 指令解析）── */
    rps_str_t       host;               /* 后端主机名或 IP */
    rps_uint_t      port;               /* 后端端口（默认 80） */

    rps_int_t       weight;             /* 轮询权重（默认 1） */
    rps_uint_t      max_fails;          /* 最大连续失败次数，超限后冷却（默认 1） */
    rps_msec_t      fail_timeout;       /* 失败冷却时间，毫秒（默认 10000 = 10s） */

    /* ── WRR 运行时状态（每次 select_peer 更新）── */
    rps_int_t       current_weight;     /* 当前动态权重（选择时 += effective，选中后 -= total） */
    rps_int_t       effective_weight;   /* 生效权重（初始 = weight，失败时下调） */
    rps_uint_t      fails;              /* 当前连续失败计数 */
    rps_msec_t      failed_time;        /* 最近一次失败的时间戳 */

    /* ── 标志 ── */
    unsigned        down:1;             /* server ... down  — 永久摘除 */
    unsigned        backup:1;           /* server ... backup — 仅当无主节点可用时使用 */
};

/*
 * 缓存的后端连接（keepalive 空闲连接栈 LIFO 中的一项）
 *
 * 存入 rps_upstream_conf_t::free_peers 数组，空闲超时后由
 * rps_upstream_cache_idle_handler 移除并关闭。
 */
typedef struct {
    rps_connection_t    *connection;    /* 空闲的后端连接（fd 仍存活、无活跃 IO） */
    rps_uint_t           requests;      /* 保留字段（实际计数在 peer->upstream_requests） */
    rps_msec_t           idle_since;    /* 放入缓存的时间戳（毫秒） */
} rps_upstream_cached_peer_t;

/*
 * upstream 组配置 — 对应一个 upstream {} 块
 *
 * 全部字段在配置解析阶段填充，free_peers 在运行时动态增减。
 * 单个 upstream 块可以包含多个 peer，通过 select_peer 做负载均衡。
 */
struct rps_upstream_conf_s {
    rps_str_t       name;               /* upstream 块名称（如 "backend"） */

    rps_array_t     peers;              /* rps_upstream_peer_t[] — 所有后端节点 */

    /* ── keepalive 连接池配置 ── */
    rps_uint_t      keepalive;          /* 最大空闲连接缓存数（0 = 禁用 keepalive) */
    rps_msec_t      keepalive_timeout;  /* 空闲连接超时时间，毫秒（默认 60000 = 60s） */
    rps_uint_t      keepalive_requests; /* 单连接最大复用请求数，超限后关闭（默认 100） */

    /* ── 负载均衡算法 ── */
    rps_upstream_peer_t *(*select_peer)(rps_upstream_conf_t *ucf);

    /* ── keepalive 空闲连接缓存（LIFO 栈，运行时填充）── */
    rps_array_t     free_peers;         /* rps_upstream_cached_peer_t[] */
};

/*
 * 运行时 upstream 上下文（per-request，从 r->pool 分配）
 *
 * 生命周期：rps_upstream_create → … → rps_upstream_finalize → request pool 销毁时释放
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
 *   body 转发:
 *     read_handler 收完头后调用 u->forward_body(r, u)
 *     → proxy 模块走 HTTP 过滤链 (rps_http_send_body)
 *     → WebSocket 模块直接透传 fd
 *
 *   对端关闭 / 出错 / 超时:
 *     → rps_upstream_finalize(r, rc)
 */
struct rps_upstream_s {
    /* ── 连接 ── */
    rps_connection_t            *peer;          /* 后端连接对象（fd + read/write 事件） */
    rps_upstream_addr_t          peer_addr;     /* 对端地址（proxy_pass 或 select_peer 填充） */
    rps_upstream_conf_t         *upstream_conf; /* upstream {} 块配置，直连代理时为 NULL */

    /* ── 缓冲区 ── */
    rps_buf_t                   *request_bufs;  /* 待发送请求数据（create_request 构造） */
    size_t                       request_sent;  /* 已发送字节数（send_handler 逐次推进） */
    rps_buf_t                   *response_buf;  /* 接收缓冲区（read_handler 追加，process_header 消费） */

    /* ── 超时，毫秒（由 proxy_xxx_timeout 或 upstream 块配置覆盖）── */
    rps_msec_t                   connect_timeout; /* 连接建立超时（默认 60000 = 60s） */
    rps_msec_t                   send_timeout;    /* 发送请求超时（默认 60000） */
    rps_msec_t                   read_timeout;    /* 读取响应超时（默认 60000） */

    /* ── 协议回调（由协议模块在 rps_upstream_init 前设置）── */
    rps_upstream_proto_t        *proto;
    rps_int_t (*create_request)(rps_http_request_t *r, rps_upstream_t *u);
    rps_int_t (*process_header)(rps_http_request_t *r, rps_upstream_t *u);
    rps_int_t (*forward_body)(rps_http_request_t *r, rps_upstream_t *u);

    /* ── 响应体接收追踪（Content-Length 完成判定，不依赖后端 EOF）── */
    size_t                       content_length_n;  /* 后端 Content-Length，0=不存在/未解析 */
    size_t                       body_received;     /* 已收到 body 字节数 */

    /* ── 标记 ── */
    unsigned                    header_complete:1; /* 后端响应头是否已解析完毕 */
    unsigned                    keepalive:1;        /* 后端响应同意 keep-alive（依据版本 + Connection 头） */
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
 * 命中缓存时检查 keepalive_requests 限制，超限则关闭旧连接并新建。
 * 返回的连接已 connect 完成（非阻塞），调用者直接 add_event(write) 即可。
 */
rps_connection_t *rps_upstream_get_peer(rps_http_request_t *r, rps_upstream_t *u);

/*
 * 归还后端连接：可复用则放入缓存并设空闲定时器，否则关闭。
 * 需同时满足：配置启用 keepalive + 后端同意(Connection: keep-alive) + 缓存未满。
 */
void rps_upstream_free_peer(rps_upstream_t *u);

/* 关闭独立分配的 upstream 后端连接（归还到 cycle 级空闲链表） */
void rps_upstream_close_peer_conn(rps_connection_t *c);

extern rps_module_t rps_upstream_module;

#endif /* _RPS_UPSTREAM_H_INCLUDED_ */
