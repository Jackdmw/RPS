# Nginx HTTP 代理全流程：从配置解析到运行时

本文以 nginx 的 `proxy_pass` 反向代理为例，追踪一条 HTTP 请求从**配置解析**到**运行时完成代理**的完整执行流。内容基于 RPS（nginx 简化实现）和 nginx 源码架构。

---

## 1. 核心数据结构全景

在追踪流程之前，先建立四个核心数据结构的全貌：

### 1.1 连接对象 (`rps_connection_t`)

```
rps_connection_t {
    fd;              // 客户端 socket 文件描述符
    read;            // → rps_event_t (读事件)
    write;           // → rps_event_t (写事件)
    sockaddr;        // 远端地址
    data;            // → 指向请求对象 rps_http_request_t (活跃时)
                     //   或指向下一个空闲连接 (空闲时，构成 freelist)
    pool;            // 连接专属内存池
    listening;       // → 所属的监听 socket
    sent:1;          // 是否已发送数据
    close:1;         // 是否需关闭
}
```

**关键设计：**
- 每个连接同时持有读事件和写事件两个 `rps_event_t` 指针
- `data` 字段在连接的不同生命周期指向不同对象：空闲时是 freelist 的 next 指针，活跃时是 HTTP 请求对象
- 连接数组在进程初始化时一次性分配（数量 = `worker_connections`），通过 freelist 管理

### 1.2 读写事件 (`rps_event_t`)

```
rps_event_t {
    data;            // → rps_connection_t (反向指回所属连接)
    handler;         // 事件就绪时的回调函数指针
    write:1;         // 1 = 写事件, 0 = 读事件
    active:1;        // 是否已注册到 epoll/io_uring
    ready:1;         // 事件是否就绪
    eof:1;           // 对端关闭连接
    error:1;         // socket 错误
    timedout:1;      // 是否超时
    timer_set:1;     // 是否插入红黑树定时器
    timer;           // 红黑树节点 (超时管理)
}
```

**关键设计：**
- `handler` 函数指针是整个事件驱动模型的核心——它随请求处理进度**动态切换**
- `data` 指向所属连接，从事件可以找回连接，再从连接的 `data` 找回请求
- 读/写事件通过 `write` 标志位区分，同一连接的两个事件各有独立的 handler

### 1.3 HTTP 请求对象 (`rps_http_request_t`)

```
rps_http_request_t {
    connection;          // → rps_connection_t
    method, uri, args;   // 请求行
    host;                // Host 头
    headers_in;          // 请求头解析结果
    headers_out;         // 待发送的响应头
    request_body;        // 请求体 buf
    out_chain;           // → rps_chain_t 响应体 buffer 链
    loc_conf;            // void** 匹配到的 location 级配置数组
    phase;               // 当前阶段 (0~10)
    phase_index;         // 阶段内 handler 下标
    pool;                // 请求专属内存池
    upstream;            // → rps_http_upstream_t (有 proxy_pass 时才分配)
    read_event_handler;  // 读事件回调 (nginx 中随进度切换)
    write_event_handler; // 写事件回调 (nginx 中初始指向阶段引擎)
}
```

### 1.4 11 个 HTTP 阶段

```
POST_READ        (0)  — 刚读完请求头，realip 模块解析 X-Forwarded-For
SERVER_REWRITE   (1)  — server 级 URL 重写
FIND_CONFIG      (2)  — 内部阶段，location 匹配
REWRITE          (3)  — location 级 URL 重写
POST_REWRITE     (4)  — 内部阶段，检查是否需要重新匹配 location
PREACCESS        (5)  — 访问限流 (limit_conn, limit_req)
ACCESS           (6)  — 访问控制 (allow/deny, auth_basic)
POST_ACCESS      (7)  — 内部阶段，检查访问是否被拒绝
PRECONTENT       (8)  — 内容生成前的最后检查
CONTENT          (9)  — 生成响应内容 (proxy, fastcgi, static)
LOG              (10) — 记录访问日志
```

**proxy_pass 在 CONTENT 阶段**注册 handler。

---

## 2. 配置解析阶段

### 2.1 配置示例

```nginx
worker_processes 2;

events {
    worker_connections 1024;
    use epoll;
}

http {
    server {
        listen 80;
        server_name "example.com";

        location /api {
            proxy_pass http://backend:8080;
        }
    }
}
```

### 2.2 解析流程

```
main()
  └── rps_init_cycle()
        ├── 打开配置文件 rps.conf
        ├── 为每个 CORE 模块调用 create_conf，填入 conf_ctx[]
        └── rps_conf_parse(&conf)                    ← 配置解析主循环

rps_conf_parse()  —— token 驱动的递归下降解析器
  │
  ├── 读到 "worker_processes" → 匹配 rps_core_module 的命令表 → 调 set 函数
  │
  ├── 读到 "events" + '{'    → 匹配 rps_event_module → 进入 event 块
  │     ├── 为每个 EVENT 模块调用 create_conf
  │     ├── 读到 "worker_connections" → 存入 rps_event_conf_t.worker_connections
  │     └── 读到 '}' → 退出 event 块
  │
  ├── 读到 "http" + '{'      → 匹配 rps_http_module → 进入 http 块
  │     │
  │     ├── 为每个 HTTP 模块调用 create_main_conf()   ← 分配 main 级配置
  │     │   (如 rps_http_core_module 创建 cmcf，含 servers 数组 + phases[])
  │     │
  │     ├── 读到 "server" + '{' → 匹配 rps_http_core_module 的 server 指令
  │     │     ├── 为每个 HTTP 模块调用 create_srv_conf()  ← 分配 srv 级配置
  │     │     ├── 为每个 HTTP 模块调用 create_loc_conf()  ← 分配 srv 预设 loc
  │     │     │
  │     │     ├── 读到 "listen 80" → 存入 srv_conf，创建 rps_listening_t
  │     │     ├── 读到 "server_name" → 存入 srv_conf.server_name
  │     │     │
  │     │     ├── 读到 "location" + '/api' + '{'
  │     │     │     ├── 为每个 HTTP 模块调用 create_loc_conf()  ← 分配 loc 级配置
  │     │     │     │
  │     │     │     ├── 读到 "proxy_pass http://backend:8080"
  │     │     │     │     → 匹配 rps_http_proxy_module 的 proxy_pass 命令
  │     │     │     │     → set 函数解析 URL，存入该 location 的 loc_conf
  │     │     │     │       (rps_http_proxy_loc_conf_t.upstream_url)
  │     │     │     │
  │     │     │     └── 读到 '}' → 退出 location 块
  │     │     │
  │     │     └── 读到 '}' → 退出 server 块
  │     │
  │     └── 读到 '}' → 退出 http 块
  │           │
  │           ├── 为每个模块调 init_main_conf()     ← 填默认值
  │           │
  │           ├── 遍历所有 server：
  │           │     ├── 为每个模块调 merge_srv_conf(main, srv)    ← main → srv 继承
  │           │     └── 遍历 server 的 location：
  │           │           └── 为每个模块调 merge_loc_conf(srv, loc) ← srv → loc 继承
  │           │
  │           └── 为每个模块调 postconfiguration(cf)
  │                 │
  │                 ├── rps_http_proxy_module 在此注册：
  │                 │     rps_http_register_phase_handler(
  │                 │         RPS_HTTP_CONTENT_PHASE,
  │                 │         rps_http_proxy_handler
  │                 │     );
  │                 │
  │                 └── rps_http_core_module 在此调用：
  │                       rps_http_init_phase_handlers()
  │                       → 将二维 phases[][] 展平为一维 phase_engine.handlers[]
```

### 2.3 阶段展平：配置时 → 运行时

```
配置时 (phases[][] 二维数组):         展平后 (phase_engine.handlers[] 一维数组):
─────────────────────────────────    ─────────────────────────────────────
phases[0].handlers = [realip]   →    [0] {checker=generic,    handler=realip,  next=1}
                                     [1] {checker=rewrite,    handler=rewrite, next=2}
phases[1].handlers = [rewrite]  →    [2] {checker=find_config,handler=find_cfg,next=3}
                                     [3] {checker=rewrite, handler=loc_rewrite,next=4}
phases[2].handlers = [...]      →    ...
...                                  ...
phases[9].handlers = [proxy]    →    [N] {checker=content, handler=proxy_handler, next=N+1}
```

展平后，阶段引擎只需要维护 `r->phase_index` 一个下标，runtime 复杂度 O(1)。

---

## 3. 运行时：连接的建立

### 3.1 进程初始化

```
main()
  └── (worker 进程)
        └── rps_event_core_init_process(cycle)
              ├── 分配 connections 数组 [worker_connections 个]
              ├── 分配 reads 数组       [worker_connections 个]
              ├── 分配 writes 数组      [worker_connections 个]
              ├── 每个 connection 关联对应的 read / write 事件
              └── 构建 freelist：connections[i].data = &connections[i+1]
```

此时内存布局：

```
connections[0]          connections[1]          connections[2]
┌──────────────┐       ┌──────────────┐       ┌──────────────┐
│ fd = -1      │       │ fd = -1      │       │ fd = -1      │
│ .read  → read[0]     │ .read  → read[1]     │ .read  → read[2]
│ .write → write[0]    │ .write → write[1]    │ .write → write[2]
│ .data ────────┼────→ │ .data ────────┼────→ │ .data → NULL │
└──────────────┘       └──────────────┘       └──────────────┘
     ↑
     free_connection (指向链表头)
```

### 3.2 接受新连接

```
epoll_wait() 返回 listen fd 就绪
  │
  └── ngx_event_accept(ev)           ← ev 是监听 fd 的读事件
        │
        ├── fd = accept(listen_fd)   ← 拿到客户端 socket
        │
        ├── c = rps_get_connection(cycle)
        │     ├── 从 free_connection 链表取头部
        │     ├── free_connection 指针后移
        │     └── 为 c 创建内存池 c->pool
        │
        ├── c->fd = fd               ← 绑定客户端 fd
        ├── c->read->handler = ngx_http_init_connection
        ├── c->write->handler = ngx_http_empty_handler  (暂无数据要写)
        ├── 将 c->read 注册到 epoll (EPOLLIN)
        └── 设置 client_header_timeout 定时器 (默认 60s)
```

---

## 4. 运行时：HTTP 解析与阶段引擎

### 4.1 读事件驱动的状态机

当客户端发来数据，连接的读事件就绪：

```
epoll_wait() 返回 c->fd 有 EPOLLIN
  │
  └── c->read->handler(rev)         ← 当前 handler = ngx_http_wait_request_handler
        │
        └── ngx_http_wait_request_handler(rev)
              ├── c = rev->data      ← 从事件找回连接
              ├── 分配 header 缓冲区 (client_header_buffer_size, 默认 1KB)
              ├── r = rps_http_create_request(c)
              │     ├── r->connection = c
              │     ├── r->pool = rps_create_pool( request_pool_size )
              │     └── c->data = r  ← ★ 连接的 data 现在指向请求
              │
              ├── rev->handler = ngx_http_process_request_line  ← ★ 切换 handler！
              └── ngx_http_process_request_line(rev)            ← 立即执行新 handler
```

**handler 切换**是事件驱动的精髓——每次 handler 执行后，根据当前状态设置下一个 handler，下次事件就绪时自动走新的处理路径。

```
请求解析的 handler 切换链：

ngx_http_wait_request_handler        ← 等待客户端发来数据
  ↓ (收到数据，分配 request)
ngx_http_process_request_line        ← 状态机解析请求行 "GET /api/hello HTTP/1.1\r\n"
  ↓ (请求行解析完)
ngx_http_process_request_headers     ← 状态机解析请求头 "Host: example.com\r\n..."
  ↓ (请求头解析完)
ngx_http_process_request()           ← 收尾，启动阶段引擎
  ├── r->read_event_handler  = ngx_http_request_handler    ← 后续读数据用
  ├── r->write_event_handler = ngx_http_core_run_phases    ← 阶段引擎入口
  └── ngx_http_core_run_phases(r)                          ← 立即启动阶段引擎
```

### 4.2 阶段引擎执行

```
ngx_http_core_run_phases(r)
  │
  └── 循环：while (r->phase_index < total_handlers)
        │
        ├── ph = &phase_engine.handlers[r->phase_index]
        │
        └── rc = ph->checker(r, ph)
              │
              ├── checker 是框架提供的包装函数，内部调用 ph->handler
              │
              └── 根据 rc 决定下一步：
                    ├── RPS_OK        → r->phase_index = ph->next  (跳到下一阶段)
                    ├── RPS_DECLINED  → r->phase_index++           (本阶段下一个 handler)
                    ├── RPS_AGAIN     → return (暂停，等异步事件，phase_index 不变)
                    └── RPS_ERROR     → 结束请求
```

各阶段经过时 r->phase_index 的变化：

```
r->phase_index = 0 ──────────────────────────────────────────────────────────
  │
  ├── POST_READ:     [realip_handler]         → 都 OK，跳到 next=2
  ├── SERVER_REWRITE:[rewrite_handler]        → 都 OK，跳到 next=3
  ├── FIND_CONFIG:   [find_config_checker]    → 匹配 location /api
  │                                           → 设置 r->loc_conf = loc_ctx->loc_conf
  ├── REWRITE:       [location_rewrite]       → 都 OK
  ├── POST_REWRITE:  [post_rewrite_checker]   → URL 未变，继续
  ├── PREACCESS:     [limit_req_handler]      → 未超限
  ├── ACCESS:        [allow_deny_handler]     → 允许访问
  ├── POST_ACCESS:   [post_access_checker]    → 未被拒绝
  ├── PRECONTENT:    [mirror_handler]         → 无 mirror 配置
  │
  ├── CONTENT:       [content_checker]
  │                    └── ph->handler = rps_http_proxy_handler  ← ★ 命中代理！
  │                          │
  │                          └── 见第 5 节
  │
  └── LOG:           [log_handler]            → 写 access.log
```

---

## 5. CONTENT 阶段：代理的完整数据流

### 5.1 proxy_handler 入口

```
rps_http_proxy_handler(r)
  │
  ├── 从 r->loc_conf[proxy_module.ctx_index] 取出 proxy 配置
  │     → 得到 upstream_url = "http://backend:8080"
  │
  ├── u = ngx_http_upstream_create(r)         ← 分配 upstream 上下文
  │     ├── u->peer.get = init_upstream       ← 设置后端选择回调
  │     ├── u->conf = proxy_loc_conf          ← 超时、buffer 大小等
  │     ├── u->create_request  = rps_http_proxy_create_request
  │     ├── u->process_header  = rps_http_proxy_process_header
  │     └── u->finalize_request = rps_http_proxy_finalize_request
  │
  ├── r->upstream = u
  │
  └── ngx_http_upstream_init(r)               ← 启动 upstream 引擎
        │
        └── 返回 RPS_AGAIN                     ← ★ 暂停阶段引擎，等异步事件
```

**关键：`RPS_AGAIN` 让阶段引擎返回，不继续执行。epoll 循环继续等待事件——这次等待的是后端连接事件。**

### 5.2 连接后端

```
ngx_http_upstream_init(r)
  │
  ├── ngx_http_upstream_connect(r, u)
  │     │
  │     ├── 从 upstream{} 或直连 URL 取后端地址 backend:8080
  │     │
  │     ├── peer_c = rps_get_connection(cycle) ← 从连接池取一个空闲连接作为后端连接
  │     │
  │     ├── fd = socket(AF_INET, SOCK_STREAM)
  │     ├── rps_set_nonblocking(fd)            ← 非阻塞
  │     ├── connect(fd, backend_addr)          ← 非阻塞 connect，立即返回
  │     │     │
  │     │     └── 返回 EINPROGRESS             ← TCP 三次握手进行中
  │     │
  │     ├── peer_c->fd = fd
  │     ├── peer_c->data = r                   ← 后端连接也指向同一个请求
  │     │
  │     ├── peer_c->write->handler = ngx_http_upstream_send_request_handler
  │     ├── peer_c->read->handler  = ngx_http_upstream_process_header
  │     │
  │     ├── 将 peer_c->write 注册到 epoll (EPOLLOUT) ← 等连接建立
  │     └── 设置 connect_timeout 定时器
  │
  └── return RPS_AGAIN
```

### 5.3 发送请求到后端

```
epoll_wait() 返回 peer_c->fd 可写 (连接建立成功)
  │
  └── peer_c->write->handler = ngx_http_upstream_send_request_handler
        │
        ├── getsockopt(fd, SOL_SOCKET, SO_ERROR)  ← 确认连接成功
        │
        ├── 将 peer_c->write 从 epoll 注销 (不再需要写就绪通知)
        │
        ├── u->create_request(r)              ← 调模块回调构造请求
        │     │
        │     └── rps_http_proxy_create_request(r):
        │           ├── 构造请求行:  "GET /api/hello HTTP/1.1\r\n"
        │           ├── 构造请求头:  "Host: backend:8080\r\n"
        │           │               "X-Forwarded-For: client_ip\r\n"
        │           │               "Connection: close\r\n"
        │           ├── 如果有请求体，加入 body buf
        │           └── 组装成 rps_chain_t → 存入 u->request_bufs
        │
        ├── 循环 send(peer_c->fd, ...)        ← 非阻塞发送
        │     ├── 一次 send 没发完 → 设 peer_c->write->handler = ngx_http_upstream_send_request
        │     │                     注册 EPOLLOUT，返回 RPS_AGAIN
        │     └── 全部发完 → 继续
        │
        └── 注册 peer_c->read 到 epoll (EPOLLIN)
             peer_c->read->handler = ngx_http_upstream_process_header
             设置 read_timeout 定时器
```

### 5.4 接收后端响应头

```
epoll_wait() 返回 peer_c->fd 有 EPOLLIN (后端响应到达)
  │
  └── peer_c->read->handler = ngx_http_upstream_process_header
        │
        ├── 循环 recv(peer_c->fd, u->buffer)   ← 读入 upstream 缓冲区
        │
        ├── u->process_header(r)               ← 调模块回调解析响应
        │     │
        │     └── rps_http_proxy_process_header(r):
        │           ├── 状态机解析 "HTTP/1.1 200 OK\r\n"
        │           ├── 解析响应头 (Content-Type, Content-Length...)
        │           ├── 填充 r->headers_out.status = 200
        │           └── 返回 RPS_OK (响应头解析完)
        │
        ├── rps_http_send_header(r)             ← 发送响应头给客户端
        │     ├── 构造 "HTTP/1.1 200 OK\r\nContent-Type: ...\r\n\r\n"
        │     └── send(c->fd, header_buf)       ← 发给客户端
        │
        ├── 判断转发模式：
        │     ├── buffering=1 → ngx_http_upstream_process_body_buffered
        │     └── buffering=0 → ngx_http_upstream_process_body_non_buffered
        │
        └── 切换 handler：
              peer_c->read->handler = ngx_http_upstream_process_body_*
```

### 5.5 转发响应体 (缓冲模式)

```
ngx_http_upstream_process_body_buffered(rev)
  │
  └── 循环：while (有数据或未完成)
        │
        ├── 从后端 recv() → 写入 u->buffer / 临时文件
        │     ├── 缓冲区满 → 写到临时文件
        │     └── 后端数据读完 → 标记 downstream_pending
        │
        ├── 如果 buffer 有足够数据 (或后端已关闭写端)：
        │     │
        │     ├── 将 buffer 数据通过 rps_chain_t 链入 r->out_chain
        │     │
        │     └── r->write_event_handler = ngx_http_core_run_phases
        │         注册 c->write 到 epoll (EPOLLOUT)  ← 等客户端可写
        │         返回 RPS_AGAIN
        │
        └── (客户端可写时)
              send(c->fd, buf)                   ← 发给客户端
              ├── 没发完 → 继续等 EPOLLOUT
              └── 发完 & 后端还有数据 → 继续从后端读 (回到循环开头)
```

**缓冲模式的数据路径：**
```
后端 ──recv──→ [u->buffer / 临时文件] ──send──→ 客户端
                   ↑ 内存/磁盘缓冲 ↑
```

**直通模式的数据路径：**
```
后端 ──recv──→ [固定大小 buffer] ──send──→ 客户端
                   ↑ 仅一个固定 buffer ↑
```

### 5.6 收尾

```
ngx_http_upstream_finalize_request(r, rc)
  │
  ├── u->finalize_request(r, rc)       ← 模块清理回调
  │     └── rps_http_proxy_finalize_request():
  │           ├── 释放 proxy 相关的临时资源
  │           └── 记录 upstream 状态 (耗时、字节数、状态码)
  │
  ├── rps_free_connection(peer_c)      ← 归还后端连接到连接池
  │
  └── ngx_http_core_run_phases(r)      ← ★ 重新进入阶段引擎
        │
        └── phase_index 推进到 LOG 阶段
              └── log_handler 写 access.log
                    └── rps_http_close_request(r)
                          ├── 销毁 r->pool (释放所有请求相关内存)
                          └── rps_free_connection(c)  ← 归还客户端连接
```

---

## 6. 完整执行流总览（以上述配置为例）

```
客户端请求: GET /api/hello HTTP/1.1
            Host: example.com

══════════════════════════════════════════════════════════════
                    配置解析阶段 (启动时，只执行一次)
══════════════════════════════════════════════════════════════

main() → rps_init_cycle() → rps_conf_parse()
  ├── worker_processes 2;       → core_conf.daemon = 1
  ├── events { worker_connections 1024; use epoll; }
  │     → event_conf.worker_connections = 1024
  │     → 分配 1024 个 connection + read event + write event
  └── http { server { listen 80; location /api { proxy_pass http://backend:8080; } } }
        → 创建 main_conf (含 phases[11])
        → 创建 srv_conf (server_name="example.com", port=80)
        → 创建 loc_conf (pattern="/api", proxy_url="http://backend:8080")
        → merge: main → srv → loc
        → postconfiguration: proxy 模块注册 CONTENT handler
        → 展平 phases 到 phase_engine.handlers[]

══════════════════════════════════════════════════════════════
                        运行时阶段
══════════════════════════════════════════════════════════════

[1] 监听
    rps_open_listening_sockets()
      └── socket() + bind() + listen() → listen_fd=3
          注册到 epoll (EPOLLIN), handler = ngx_event_accept

[2] epoll_wait: listen_fd 就绪
    ngx_event_accept(ev)
      ├── fd = accept() → client_fd=7
      ├── c = rps_get_connection()  ← 从 freelist 取
      ├── c->fd = 7
      ├── c->read->handler = ngx_http_wait_request_handler
      └── epoll_ctl(ADD, 7, EPOLLIN)

[3] epoll_wait: client_fd=7 可读 (客户端发来请求)
    c->read->handler = ngx_http_wait_request_handler
      ├── 分配 rps_http_request_t → r
      ├── c->data = r
      ├── rev->handler = ngx_http_process_request_line  ← 切换 handler
      └── → 状态机解析 "GET /api/hello HTTP/1.1\r\n"

[4] 请求行解析完 → rev->handler = ngx_http_process_request_headers
    → 状态机解析 "Host: example.com\r\n..."

[5] 请求头解析完
    ngx_http_process_request()
      ├── r->write_event_handler = ngx_http_core_run_phases
      └── → ngx_http_core_run_phases(r)

[6] 阶段引擎执行:
    ┌──────────┬─────────────────────┬─────────────────┐
    │ 阶段      │ handler             │ 结果             │
    ├──────────┼─────────────────────┼─────────────────┤
    │ POST_READ    │ (无 handler)        │ 跳过              │
    │ SVR_REWRITE  │ (无 handler)        │ 跳过              │
    │ FIND_CONFIG  │ find_config_checker │ 匹配 /api location│
    │              │                     │ r->loc_conf = loc │
    │ REWRITE      │ (无 handler)        │ 跳过              │
    │ POST_REWRITE │ (无 handler)        │ 跳过              │
    │ PREACCESS    │ (无 handler)        │ 跳过              │
    │ ACCESS       │ (无 handler)        │ 跳过              │
    │ POST_ACCESS  │ (无 handler)        │ 跳过              │
    │ PRECONTENT   │ (无 handler)        │ 跳过              │
    │ CONTENT      │ proxy_handler       │ ★ 进入代理流程    │
    └──────────┴─────────────────────┴─────────────────┘

[7] CONTENT: proxy_handler
    ├── 从 loc_conf 拿到 upstream_url = "http://backend:8080"
    ├── 分配 rps_http_upstream_t → u
    ├── u->create_request  = proxy_create_request
    ├── u->process_header  = proxy_process_header
    └── ngx_http_upstream_init(r) → 返回 RPS_AGAIN

[8] 连接后端
    ngx_http_upstream_connect()
      ├── peer_c = rps_get_connection()  ← 从连接池取后端连接
      ├── fd = socket() → backend_fd=8
      ├── connect(8, backend:8080) → EINPROGRESS
      ├── peer_c->read->handler  = upstream_process_header
      ├── peer_c->write->handler = upstream_send_request
      └── epoll_ctl(ADD, 8, EPOLLOUT)

[9] epoll_wait: backend_fd=8 可写 (连接建立)
    upstream_send_request_handler
      ├── u->create_request(r) → 构造 "GET /api/hello HTTP/1.1\r\n..."
      ├── send(8, request_buf) → 请求发出
      ├── epoll_ctl(DEL, 8, EPOLLOUT)
      └── epoll_ctl(ADD, 8, EPOLLIN)

[10] epoll_wait: backend_fd=8 可读 (后端响应)
     upstream_process_header
       ├── recv(8, buf) → "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n..."
       ├── u->process_header(r) → 解析出 status=200, content_type=application/json
       ├── rps_http_send_header(r) → send(7, "HTTP/1.1 200 OK\r\n...") 给客户端
       └── rev->handler = upstream_process_body  ← 切换为 body 接收

[11] 转发 body
     upstream_process_body
       ├── recv(8, buf) → 读后端 body
       └── send(7, buf) → 转发给客户端
       (循环直到后端关闭写端)

[12] 收尾
     ngx_http_upstream_finalize_request(r, RPS_OK)
       ├── u->finalize_request(r) → proxy 清理
       ├── rps_free_connection(peer_c) → 归还后端连接
       └── → ngx_http_core_run_phases(r) 继续阶段引擎

[13] LOG 阶段
     log_handler → 写 access.log
       └── rps_http_close_request(r) → 销毁请求内存池
       └── rps_free_connection(c) → 归还客户端连接

══════════════════════════════════════════════════════════════
                        连接池状态变化
══════════════════════════════════════════════════════════════

初始: free_connection → [0] → [1] → [2] → ... → [1023] → NULL

[2] 客户端连接: free_connection → [1] → [2] → ...
    connections[0] 被占用 (client_fd=7)

[8] 后端连接:   free_connection → [2] → [3] → ...
    connections[0] 被占用 (client_fd=7)
    connections[1] 被占用 (backend_fd=8)

[12] 后端归还: free_connection → [2] → [3] → ...
    connections[0] 被占用 (client_fd=7)
    connections[1] → free

[13] 客户端归还: free_connection → [1] → [2] → [3] → ...
    全部归还
```

---

## 7. 关键设计总结

### 7.1 handler 切换表

| 阶段 | rev->handler (读事件) | wev->handler (写事件) |
|---|---|---|
| 等待请求 | `wait_request_handler` | `empty_handler` |
| 解析请求行 | `process_request_line` | `empty_handler` |
| 解析请求头 | `process_request_headers` | `empty_handler` |
| 阶段引擎中 | `request_handler` | `core_run_phases` |
| upstream 连接中 | `upstream_process_header` | `upstream_send_request` |
| upstream 转发 body | `upstream_process_body` | `upstream_send_body` |

**每次 handler 执行完后，可以原地修改自己的函数指针来决定"下次这个事件就绪时调谁"。** 这就是事件驱动状态机的核心。

### 7.2 内存管理

```
cycle->pool         → 进程级 (连接数组、事件数组)
  c->pool           → 连接级 (sockaddr、地址文本)
    r->pool         → 请求级 (headers、body、chain、upstream)
      (请求结束一次性释放，所有子结构跟随销毁)
```

### 7.3 阶段引擎如何驱动异步操作

阶段引擎 + upstream 配合的核心模式：

1. proxy_handler 启动 upstream → 返回 `RPS_AGAIN`
2. 阶段引擎收到 `RPS_AGAIN` → 保存 `phase_index`，返回 epoll 循环
3. 后端连接就绪 → 触发 upstream handler → 发送请求 / 接收响应
4. 全部完成 → 调用 `ngx_http_core_run_phases(r)` → 从上次的 `phase_index` 继续

这样就实现了"在 CONTENT 阶段中间暂停，等后端数据到了再继续"的效果，全程没有阻塞任何线程。

### 7.4 RPS 代码映射

| 概念 | nginx 源文件 | RPS 对应文件 |
|---|---|---|
| 连接对象 | `ngx_connection.h` | `src/core/rps_connection.h` |
| 事件对象 | `ngx_event.h` | `src/event/rps_event.h` |
| 请求对象 | `ngx_http_request.h` | `src/http/rps_http_core.h:57-84` |
| 阶段定义 | `ngx_http_core_module.h` | `src/http/rps_http_core.h:91-103` |
| 阶段引擎 | `ngx_http_core_module.c` | `src/http/rps_http_core.h:122` (声明) |
| 配置解析 | `ngx_conf_file.c` | `src/core/rps_conf_file.c` |
| upstream 引擎 | `ngx_http_upstream.c` | (待实现) |
| proxy 模块 | `ngx_http_proxy_module.c` | (待实现) |
