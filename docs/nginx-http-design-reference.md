# Nginx HTTP 模块设计参考

本文档整理 nginx HTTP 子系统的完整设计，作为 RPS HTTP 模块实现的参照。

---

## 1. 整体参与链路：从 socket 到阶段引擎

一个连接从内核到 HTTP 处理完毕的完整链路：

```
epoll_wait
  └── ngx_event_accept()                          # 监听 fd 就绪
        ├── accept()                               # 拿到客户端 fd
        └── ls->handler(c)                         # → ngx_http_init_connection()

ngx_http_init_connection()
  ├── 分配 ngx_http_connection_t，存入 c->data
  ├── 保存 hc->addr_conf（用于后续虚拟主机分发）
  ├── rev->handler = ngx_http_wait_request_handler
  ├── 添加 client_header_timeout 定时器（默认 60s）
  └── 将 fd 注册到 epoll 等待客户端数据

ngx_http_wait_request_handler()                   # 客户端发来数据
  ├── 分配 header 缓冲区
  ├── 分配 rps_http_request_t
  ├── c->data = r  (替换掉 hc)
  ├── rev->handler = ngx_http_process_request_line
  └── 进入请求行解析（状态机）

ngx_http_process_request_line() → 请求头解析
  └── ngx_http_process_request_headers()

ngx_http_process_request()                         # 解析完成，启动阶段引擎
  ├── r->read_event_handler  = ngx_http_request_handler
  ├── r->write_event_handler = ngx_http_core_run_phases
  └── ngx_http_core_run_phases(r)                 # 11 阶段处理
```

---

## 2. 核心数据结构

### 2.1 rps_http_request_t — 请求对象

整个 HTTP 子系统围绕这个结构运转。一个连接至少分配一个 request，子请求会产生 request 树。

```
rps_http_request_t {
    // ── 关联关系 ──
    connection;               /* rps_connection_t *   */
    ctx;                      /* void ** 每个模块的请求上下文，按 ctx_index 索引 */
    main_conf / srv_conf / loc_conf;  /* void ** 三级配置指针数组 */
    pool;                     /* 请求专属内存池 */
    http_connection;          /* rps_http_connection_t * 连接级 HTTP 数据 */

    // ── 事件驱动钩子 ──
    read_event_handler;       /* 读事件回调，随处理进度切换 */
    write_event_handler;      /* 写事件回调，初始指向 ngx_http_core_run_phases */

    // ── 请求行 ──
    method / method_name;     /* HTTP 方法，枚举 + 字符串 */
    http_version;             /* HTTP/1.0 或 HTTP/1.1 */
    request_line;             /* 原始请求行（不修改） */
    unparsed_uri;             /* 原始 URI（不修改，指向接收缓冲） */
    uri;                      /* 解码后的 URI（rewrite 可以改） */
    args;                     /* query string */
    exten;                    /* 文件扩展名，如 "html" */

    // ── 请求头 (headers_in) ──
    headers;                  /* rps_list_t — 所有请求头的链表 */
    host / user_agent / content_length / content_type / ...;  /* 常用头的快速指针 */
    content_length_n;         /* Content-Length 的数字值 */
    cookies;                  /* 预解析的 cookie 数组 */

    // ── 响应头 (headers_out) ──
    status;                   /* HTTP 状态码 */
    headers;                  /* rps_list_t — 所有响应头的链表 */
    content_type / location / ...;  /* 常用响应头的快速指针 */

    // ── 请求体 ──
    request_body;             /* rps_http_request_body_t * — bufs/temp_file */
    request_body_in_single_buf / request_body_in_file_only / ...; /* 标志位 */

    // ── 阶段引擎状态 ──
    phase_handler;            /* 当前在 phase_engine.handlers[] 中的下标 */
    content_handler;          /* 从 clcf->handler 复制来的内容生成函数 */

    // ── 输出 ──
    out;                      /* rps_chain_t * — 待发送的响应数据链 */
    header_sent:1;            /* 响应头是否已发送 */
    buffered:4;               /* 各 filter 还有待 flush 的数据 */

    // ── 子请求 ──
    main;                     /* 顶层请求（r == r->main 说明不是子请求） */
    parent;                   /* 父请求 */
    postponed;                /* 等待父请求处理的缓冲数据 */
    count:8;                  /* 引用计数（子请求 +1，完成后 -1，归零才真正结束） */
    subrequests:8;            /* 剩余可创建的子请求数（默认 200） */

    // ── upstream ──
    upstream;                 /* rps_http_upstream_t * — 有 proxy_pass 时才非空 */

    // ── 标志位 ──
    keepalive:1 / internal:1 / header_only:1 / chunked:1 / pipeline:1 /
    uri_changed:1 / done:1 / logged:1 / error_page:1 / ...

    // ── 清理与计时 ──
    cleanup;                  /* rps_http_cleanup_t * — 请求销毁时的清理回调链 */
    start_sec / start_msec;   /* 请求创建时间 */
}
```

**功能要点：**

| 功能 | 如何实现 |
|---|---|
| 请求状态驱动 | `read_event_handler` / `write_event_handler` 函数指针随阶段切换 |
| 模块私有数据 | `ctx[module.ctx_index]` 指针数组，每个模块在请求中挂自己的状态 |
| 配置访问 | `main_conf/srv_conf/loc_conf[module.ctx_index]` 拿到合并后的最终配置 |
| 内存管理 | `pool` 是请求专属内存池，请求结束一次性释放 |
| 子请求生命周期 | `count` 引用计数，为 0 才最终结束 |
| 响应构造 | `out` 是 buffer 链表，filter 链依次处理 |

### 2.2 rps_http_phase_handler_t — 阶段引擎的原子单元

```c
typedef struct {
    rps_http_phase_handler_pt  checker;  /* 框架提供的包装函数 */
    rps_http_handler_pt        handler;  /* 模块提供的业务逻辑 */
    rps_uint_t                 next;     /* 下一阶段首个 handler 的下标 */
} rps_http_phase_handler_t;
```

- `checker` — 由 http_core 提供。它调用 `handler`，根据返回值决定：继续本阶段 / 跳到下阶段 / 结束请求
- `handler` — 由模块提供（如 proxy_handler），做实际工作
- `next` — 跳阶段时直接 `r->phase_handler = ph->next`

不同阶段有不同 checker：

| Checker | 负责的阶段 |
|---|---|
| `generic_phase` | POST_READ, PREACCESS, PRECONTENT, LOG |
| `rewrite_phase` | SERVER_REWRITE, REWRITE |
| `find_config_phase` | FIND_CONFIG |
| `post_rewrite_phase` | POST_REWRITE |
| `access_phase` | ACCESS |
| `post_access_phase` | POST_ACCESS |
| `try_files_phase` | TRY_FILES |
| `content_phase` | CONTENT |

### 2.3 rps_http_phase_engine_t

配置初始化时，二维的 `phases[]` 数组被展平成一维数组：

```c
typedef struct {
    rps_http_phase_handler_t  *handlers;          /* 展平后的一维数组 */
    rps_uint_t                 server_rewrite_index;  /* SERVER_REWRITE 起始下标 */
    rps_uint_t                 location_rewrite_index;/* REWRITE 起始下标 */
} rps_http_phase_engine_t;
```

### 2.4 rps_http_phase_t — 配置时的阶段表示

```c
typedef struct {
    rps_array_t  handlers;   /* rps_http_handler_pt 数组 */
} rps_http_phase_t;
```

模块在 `postconfiguration` 中往 `phases[阶段].handlers` 里追加 handler 函数指针。

### 2.5 三级配置容器

```c
typedef struct {
    void **main_conf;  /* 指针数组，main_conf[module.ctx_index] */
    void **srv_conf;   /* 指针数组，srv_conf[module.ctx_index] */
    void **loc_conf;   /* 指针数组，loc_conf[module.ctx_index] */
} rps_http_conf_ctx_t;

// 请求时取值：
#define rps_http_get_module_loc_conf(r, module)  (r->loc_conf[module.ctx_index])
```

http 级的 ctx 只有 `main_conf`（srv/loc 为 NULL）。每个 server 和每个 location 各有一个 ctx。

### 2.6 http_core 模块的三级配置

**main_conf — 只有 core 模块有 servers 数组和阶段引擎：**

```c
typedef struct {
    rps_array_t                servers;          /* rps_http_conf_ctx_t * 数组 */
    rps_http_phase_engine_t    phase_engine;     /* 展平后的阶段处理器 */
    rps_http_phase_t           phases[12];       /* 11 个阶段 + LOG，模块往里推 handler */
    rps_array_t               *ports;            /* 按端口组织的监听地址 */
    rps_hash_t                 headers_in_hash;  /* 请求头名 → 解析函数的哈希 */
    rps_uint_t                 server_names_hash_max_size;
    rps_uint_t                 server_names_hash_bucket_size;
} rps_http_core_main_conf_t;
```

**srv_conf — 每个 server{} 块一份：**

```c
typedef struct {
    rps_str_t                  server_name;      /* 主 server_name */
    rps_uint_t                 connection_pool_size;
    rps_uint_t                 request_pool_size;
    rps_uint_t                 client_header_buffer_size;
    rps_http_conf_ctx_t       *ctx;              /* 指回该 server 的完整容器 */
    rps_array_t               *server_names;     /* 所有 server_name */
    rps_list_t                 locations;        /* 该 server 下的所有 location */
} rps_http_core_srv_conf_t;
```

**loc_conf — 每个 location{} 块一份：**

```c
typedef struct {
    rps_str_t                  name;             /* location 的路径，如 "/api/" */
    rps_str_t                  root;             /* 文档根目录 */
    rps_str_t                  alias;            /* alias 替换 */
    rps_http_handler_pt        handler;          /* content handler（proxy_pass 指定的） */
    rps_http_conf_ctx_t       *loc_conf;         /* 指向该 loc 的完整容器 */

    unsigned                   exact_match:1;    /* = /path 精确匹配 */
    unsigned                   noregex:1;        /* ^~ 不搜索正则 */

    /* location 树 */
    rps_http_location_tree_node_t  *static_locations; /* 前缀匹配树 */
    rps_http_core_loc_conf_t      **regex_locations;  /* 正则 location 数组 */
} rps_http_core_loc_conf_t;
```

### 2.7 rps_http_headers_in_t — 请求头

```c
typedef struct {
    rps_list_t       headers;          /* rps_table_elt_t 链表（全部请求头） */

    /* 常用头的快速指针，指向 headers 链表中的对应节点 */
    rps_table_elt_t *host;
    rps_table_elt_t *user_agent;
    rps_table_elt_t *content_type;
    rps_table_elt_t *content_length;
    rps_table_elt_t *connection;
    rps_table_elt_t *transfer_encoding;
    rps_table_elt_t *authorization;
    rps_table_elt_t *cookie;

    off_t            content_length_n; /* 数字化的 Content-Length */
    rps_array_t      cookies;          /* 预解析的 cookie 键值对 */
} rps_http_headers_in_t;

// 单个请求头 / 响应头条目
typedef struct {
    rps_str_t   key;
    rps_str_t   value;
} rps_table_elt_t;
```

### 2.8 rps_http_headers_out_t — 响应头

```c
typedef struct {
    rps_list_t       headers;
    rps_uint_t       status;
    rps_str_t        content_type;
    off_t            content_length_n;
    rps_table_elt_t *server;
    rps_table_elt_t *date;
    rps_table_elt_t *location;
    rps_table_elt_t *content_range;
} rps_http_headers_out_t;
```

### 2.9 rps_chain_t — Buffer 链

```c
typedef struct rps_chain_s {
    rps_buf_t           *buf;
    struct rps_chain_s  *next;
} rps_chain_t;
```

请求头和响应头用单个 buf，响应体可能用 buffer 链构造（如 proxy 模块拼接后端响应头 + body）。

### 2.10 rps_http_upstream_t — 上游请求上下文

只有在使用 `proxy_pass` 等反向代理指令时才会分配：

```c
typedef struct {
    // ── 事件驱动 ──
    rps_http_upstream_handler_pt  read_event_handler;
    rps_http_upstream_handler_pt  write_event_handler;

    // ── 后端连接 ──
    rps_peer_connection_t         peer;    /* 封装了后端 fd、状态 */
    rps_http_upstream_conf_t     *conf;    /* 超时、缓冲区大小等配置 */
    rps_http_upstream_srv_conf_t *upstream;/* upstream{} 块定义的服务器组 */

    // ── 模块回调 ──
    rps_int_t (*create_request)(rps_http_request_t *r);       /* 构造发往后端的请求 */
    rps_int_t (*process_header)(rps_http_request_t *r);       /* 解析后端响应头 */
    void      (*finalize_request)(rps_http_request_t *, rps_int_t rc); /* 清理 */
    rps_int_t (*reinit_request)(rps_http_request_t *r);       /* 重试前重置状态 */
    void      (*abort_request)(rps_http_request_t *r);        /* 中止请求 */

    // ── 缓冲区 ──
    rps_buf_t                     buffer;         /* 接收缓冲区 */
    rps_chain_t                  *request_bufs;   /* 发往后端的请求数据 */

    // ── 状态 ──
    rps_http_upstream_state_t    *state;          /* 当前尝试的耗时和状态 */
    rps_uint_t                    request_sent:1; /* 请求是否已发出 */
} rps_http_upstream_t;
```

### 2.11 rps_http_upstream_srv_conf_t — upstream{} 块配置

```c
typedef struct {
    rps_http_upstream_peer_t  *peers;     /* 后端服务器列表 */
    rps_uint_t                 n;         /* 后端数量 */
    void                     **srv_conf;  /* 各模块的 upstream 级配置 */
} rps_http_upstream_srv_conf_t;
```

### 2.12 rps_http_upstream_conf_t — proxy 模块的配置

```c
typedef struct {
    rps_msec_t  connect_timeout;
    rps_msec_t  send_timeout;
    rps_msec_t  read_timeout;
    size_t      buffer_size;               /* 接收响应头的缓冲区大小 */
    size_t      busy_buffers_size;         /* 缓冲模式下忙缓冲最大大小 */
    size_t      max_temp_file_size;        /* 临时文件最大大小 */
    rps_uint_t  buffering:1;               /* 是否启用缓冲（否则直通） */
    rps_uint_t  pass_request_headers:1;
    rps_uint_t  pass_request_body:1;
    rps_uint_t  intercept_errors:1;        /* 是否拦截后端错误 */
    rps_uint_t  next_upstream;             /* 哪些错误可重试（位掩码） */
    rps_uint_t  next_upstream_tries;       /* 最大重试次数 */
} rps_http_upstream_conf_t;
```

### 2.13 rps_http_module_ctx_t — HTTP 模块上下文接口

```c
typedef struct {
    rps_int_t (*preconfiguration)(rps_conf_t *cf);
    rps_int_t (*postconfiguration)(rps_conf_t *cf);

    void *(*create_main_conf)(rps_conf_t *cf);
    char *(*init_main_conf)(rps_conf_t *cf, void *conf);

    void *(*create_srv_conf)(rps_conf_t *cf);
    char *(*merge_srv_conf)(rps_conf_t *cf, void *prev, void *conf);

    void *(*create_loc_conf)(rps_conf_t *cf);
    char *(*merge_loc_conf)(rps_conf_t *cf, void *prev, void *conf);
} rps_http_module_ctx_t;
```

---

## 3. 11 个处理阶段

### 阶段定义

```c
typedef enum {
    RPS_HTTP_POST_READ_PHASE      = 0,
    RPS_HTTP_SERVER_REWRITE_PHASE = 1,
    RPS_HTTP_FIND_CONFIG_PHASE    = 2,
    RPS_HTTP_REWRITE_PHASE        = 3,
    RPS_HTTP_POST_REWRITE_PHASE   = 4,
    RPS_HTTP_PREACCESS_PHASE      = 5,
    RPS_HTTP_ACCESS_PHASE         = 6,
    RPS_HTTP_POST_ACCESS_PHASE    = 7,
    RPS_HTTP_TRY_FILES_PHASE      = 8,
    RPS_HTTP_PRECONTENT_PHASE     = 9,
    RPS_HTTP_CONTENT_PHASE        = 10,
    RPS_HTTP_LOG_PHASE            = 11,
} rps_http_phases;
```

### 各阶段职责

| 阶段 | 用途 | 谁在此注册 |
|---|---|---|
| POST_READ | 刚读完请求头 | realip 模块（解析 X-Forwarded-For） |
| SERVER_REWRITE | server 级 URL 重写 | rewrite 模块（server 块内的 rewrite 指令） |
| FIND_CONFIG | **内部阶段**，匹配 location | 只有 http_core，无模块注册 |
| REWRITE | location 级 URL 重写 | rewrite 模块（location 块内的 rewrite 指令） |
| POST_REWRITE | **内部阶段**，检查是否需重新匹配 location | 只有 http_core |
| PREACCESS | 访问限流 | limit_conn, limit_req 模块 |
| ACCESS | 访问控制 | access 模块 (allow/deny), auth_basic, auth_request |
| POST_ACCESS | **内部阶段**，检查访问是否被拒绝 | 只有 http_core |
| TRY_FILES | **内部阶段**，处理 try_files | 只有 http_core |
| PRECONTENT | 内容生成前的最后检查 | mirror, random_index, subrequest 模块 |
| CONTENT | **生成响应内容** | proxy, fastcgi, uwsgi, static 模块 |
| LOG | 记录访问日志 | log 模块 |

### handler 返回值约定

| 返回值 | 含义 |
|---|---|
| `RPS_OK` | 处理完成，跳到下一阶段 |
| `RPS_DECLINED` | 不处理，本阶段下一个 handler 继续 |
| `RPS_AGAIN` | 等待异步事件，暂停（不修改 phase_handler） |
| `RPS_ERROR` | 出错，结束请求 |
| CONTENT 阶段的 `RPS_OK` | 已成功生成内容，不再调用后续 handler |

### 阶段展平

`rps_http_init_phase_handlers()` 在配置初始化时执行：

```
配置时：                          运行时：
phases[0].handlers = [a, b]      handlers[0] = {checker=generic,    handler=a, next=2}
phases[1].handlers = [c]    →    handlers[1] = {checker=generic,    handler=b, next=2}
phases[2].handlers = [d, e]      handlers[2] = {checker=rewrite,    handler=c, next=3}
                                 handlers[3] = {checker=find_config, handler=d, next=4}
                                 handlers[4] = {checker=rewrite,    handler=e, next=5}
                                 ...
```

运行时 `r->phase_handler` 是一个下标，checker 决定是 `+1`（继续本阶段）还是跳到 `ph->next`（下一阶段）。

---

## 4. 虚拟主机和 Location 匹配

### 监听端口结构

```
cmcf->ports[]  (每个 listen 端口一个)
  └── ngx_http_conf_port_t
       ├── addrs[]  (该端口上的每个 IP 一个)
       │    └── ngx_http_conf_addr_t
       │         ├── default_server → 默认 server 配置
       │         ├── hash → 精确主机名哈希表
       │         ├── wc_head → *.example.com 通配哈希
       │         └── wc_tail → example.* 通配哈希
       └── servers[] → 所有监听此端口的 ngx_http_core_srv_conf_t
```

### Location 匹配算法

```
1. 遍历所有前缀 location，记录最长匹配
2. 如果最长匹配是 exact_match (=)，立即使用，停止
3. 如果最长匹配设置了 noregex (^~)，跳过正则，使用此前缀匹配
4. 否则按声明顺序遍历所有 ~regex~ location，第一个匹配的生效
5. 如果正则无匹配，使用之前的最长前缀匹配
```

### Location 树

前缀 location 被构造成一棵平衡查找树（`rps_http_location_tree_node_t`），在 FIND_CONFIG 阶段做 O(log n) 前缀匹配。

---

## 5. 模块配置生命周期

### 解析 a 阶段（进入 http{} 块时）

```
ngx_http_block():
  for each HTTP module:
    module->create_main_conf(cf)       → 分配 main 级配置，填 UNSET
    (为 http 级预设一个 loc_conf)

  cf->cmd_type = RPS_HTTP_MAIN_CONF
  解析 http{} 内的所有指令：

    遇到 server {:
      for each HTTP module:
        module->create_srv_conf(cf)    → 分配 srv 级配置
        module->create_loc_conf(cf)    → 分配 loc 级配置（server 级预设值）

        解析 server{} 内的所有指令：
          遇到 location /path {:
            for each HTTP module:
              module->create_loc_conf(cf) → 分配 loc 级配置

            解析 location{} 内的所有指令...
```

### 解析后阶段（postconfiguration）

```
for each module:
  module->init_main_conf(cf, conf)      // 给 main_conf 填默认值

for each server:
  for each module:
    module->merge_srv_conf(cf, main_conf, srv_conf)  // main → server 继承

  for each location in server:
    for each module:
      module->merge_loc_conf(cf, srv_conf, loc_conf) // server → location 继承

for each module:
  module->postconfiguration(cf)         // 注册阶段 handler、初始化阶段引擎
```

---

## 6. Upstream / Proxy 数据流

### 完整流程

```
CONTENT 阶段 → rps_http_proxy_handler(r)
  │
  ├── ngx_http_upstream_create(r)          # 分配 u，设置 u->conf、u->callbacks
  ├── ngx_http_upstream_init(r)            # 返回 RPS_AGAIN，挂起阶段引擎
  │
  ├── [选择后端 peer]
  │   ├── u->peer.get()                    # 负载均衡选一台后端
  │   └── socket() + connect()             # 建立连接（非阻塞）
  │
  ├── [发送请求]
  │   ├── create_request(r)                # 模块构造请求 buffer
  │   └── send() → 后端                     # 非阻塞写
  │
  ├── [接收响应头]
  │   ├── recv() ← 后端
  │   ├── process_header(r)                # 模块解析响应头
  │   └── 发送响应头给客户端
  │
  ├── [接收/转发 body]
  │   ├── 缓冲模式：读入内存/临时文件 → 转发
  │   └── 直通模式：边读边发
  │
  └── [结束]
      ├── finalize_request(r, rc)          # 模块清理
      └── 归还后端连接到 peer pool
```

### 两种转发模式

| | 缓冲模式 (buffering=1) | 直通模式 (buffering=0) |
|---|---|---|
| 数据路径 | 后端 → 内存/临时文件 → 客户端 | 后端 → 固定大小 buffer → 客户端 |
| 内存消耗 | 高（可能写临时文件） | 低 |
| 适用 | 后端快、客户端慢 | 后端和客户端速度接近 |
| nginx 默认 | 是 | 否 |

---

## 7. 实现基础代理的最小模块清单

```
rps_http_request         基础设施 — 请求解析 + 阶段引擎 + 响应输出
rps_http_core_module     大管家 — http/server/location 块 + listen + FIND_CONFIG
rps_http_proxy_module    代理 — proxy_pass 指令 + CONTENT handler
rps_http_upstream_module 后端管理 — upstream{} 块 + peer 连接池（可先简化为直连）
```

每个模块需实现 `rps_http_module_ctx_t` 的全部或部分回调，在 `postconfiguration` 中注册阶段 handler。

---

## 8. 关键设计要点

1. **配置三层继承** — main → server → location，通过 merge 函数实现默认值向下传递
2. **request 是全局上下文** — 所有模块通过 `r->ctx[my_index]` 存取自己的请求级状态
3. **阶段引擎是核心调度器** — 所有模块的功能（rewrite、access、proxy）都化为 handler 挂在阶段上
4. **事件驱动靠回调指针切换** — `read_event_handler` / `write_event_handler` 随请求进度变化，实现状态机
5. **子请求靠引用计数** — `r->count` 归零才真正结束请求，确保异步子请求不提前释放
6. **upstream 是协议无关的反向代理引擎** — proxy/fastcgi/uwsgi 都复用同一套 upstream 框架，只实现 create_request / process_header / finalize_request 三个回调
7. **线程模式可绕开事件驱动** — handler 内部用阻塞 read/write，其余数据结构（request、headers、phase）全部复用
