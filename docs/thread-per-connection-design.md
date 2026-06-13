# 一连接一线程 模式设计

## 设计原则：最小侵入

不改 HTTP 解析、阶段引擎、配置系统、proxy 回调逻辑。只换 I/O 层。

```
事件驱动                                              线程驱动
────────                                              ────────
epoll_wait ──→ handler ──→ handler ──→ ...           epoll (只用于 accept)
   ↑                        ↓                          ↓
   └──── event loop ────────┘                    pthread_create ──→ 线程内 poll 循环
```

## 一、accept 层：复用 epoll

不变，`rps_event_accept` 照常工作。根据 `cycle->if_pthread` 决定走哪条路径：

```c
// rps.c: rps_event_accept()
// ... accept fd, get connection, create request ...

if (cycle->if_pthread) {
    // 线程模式：把 request 和 fd 丢给线程，不再注册 epoll 事件
    thread_spawn(r);
} else {
    // 事件模式：现有流程
    new_c->read->handler = rps_http_wait_request_handler;
    add_event(new_c->read, RPS_READ_EVENT);
    rps_event_add_timer(new_c->read, 60000);
}
```

## 二、连接池线程安全

三个地方加 `pthread_mutex_t`：

```c
// rps_cycle_t 新增
pthread_mutex_t  conn_mutex;          // 保护 free_connection
pthread_mutex_t  upstream_conn_mutex; // 保护 free_upstream_connections

// rps_get_connection:    lock → pop → unlock
// rps_free_connection:   lock → push → unlock
// rps_upstream_new_peer_conn: lock → pop/alloc → unlock
// rps_upstream_close_peer_conn: lock → push → unlock
```

## 三、线程 worker

```c
typedef struct {
    rps_cycle_t        *cycle;
    rps_connection_t   *c;        // 客户端连接
    rps_http_request_t *r;        // 已创建好的 request
    void              **conf_ctx; // 配置上下文指针
} thread_ctx_t;

void *thread_worker(void *arg) {
    thread_ctx_t ctx = *(thread_ctx_t *)arg;

    r = ctx.r;
    c = ctx.c;

    // 解析已在 rps_event_accept 之前的 handler 中完成?
    // 不，线程接管后自己读自己解析
    // 所以这里 request 是空的，需要线程自己读数据
    
    while (1) {   // keepalive 循环
        // ── 1. 阻塞读请求 ──
        n = read_http_request(r);
        if (n <= 0) break;

        // ── 2. 解析 ──
        解析请求行 → 解析请求头 → parse_status = 2

        // ── 3. 阶段引擎（同步执行，proxy handler 内部用阻塞 I/O）──
        rps_http_run_phases(r, cmcf);
        // 阶段引擎返回时请求已处理完毕

        // ── 4. keepalive 判定 ──
        if (!r->keepalive) break;
        
        // 重置 request 状态，准备读下一个请求
        rps_http_reset_request(r);
    }

    // 清理
    rps_http_finalize_request(r, rc);
    rps_free_connection(c);
    free(arg);
    return NULL;
}
```

### HTTP 请求阻塞读

```c
static int read_http_request(rps_http_request_t *r) {
    struct pollfd pfd;
    pfd.fd = r->connection->fd;
    pfd.events = POLLIN;

    while (1) {
        int ret = poll(&pfd, 1, 60000); // 60s 超时
        if (ret < 0) return -1;
        if (ret == 0) return -1;         // 超时

        n = recv(fd, b->last, remaining, 0);
        if (n <= 0) return n;
        b->last += n;

        // 尝试解析
        if (!parse_line_done) { parse_request_line(r); }
        if (!parse_hdr_done)  { parse_headers(r); }
        if (parse_status == 2) return 1;  // 完成
    }
}
```

## 四、HTTP 代理：阻塞版 upstream

pthread 模式下 proxy handler 走阻塞路径，不复用 `rps_upstream_init` 的事件驱动流程：

```c
static rps_int_t
rps_http_proxy_handler(rps_http_request_t *r) {
    // ... 配置查找、upstream_create、设置回调 (同现有代码) ...

    if (cycle->if_pthread) {
        // 线程模式：直接用 poll + 阻塞 I/O
        return thread_proxy_run(r, u);
    } else {
        // 事件模式：现有流程
        r->upstream = u;
        rps_upstream_init(r, u);
        return RPS_AGAIN;
    }
}

static rps_int_t
thread_proxy_run(rps_http_request_t *r, rps_upstream_t *u) {
    // ── 1. 构造请求 ──
    u->create_request(r, u);

    // ── 2. 阻塞 connect ──
    fd = socket(...); set_nonblocking(fd);
    connect(fd, ...);   // EINPROGRESS 正常
    poll(POLLOUT, timeout);  // 等连接完成
    getsockopt(SO_ERROR) 检查

    // ── 3. 发请求 ──
    while (request_sent < total) {
        poll(POLLOUT, timeout);
        n = send(fd, ...);
        request_sent += n;
    }

    // ── 4. 收响应头 ──
    while (!header_complete) {
        poll(POLLIN, timeout);
        n = recv(fd, buffer, ...);
        response_buf->last += n;
        u->process_header(r, u);  // ← 同一回调!
    }

    // ── 5. 收 body + 转发客户端 ──
    while (body_received < content_length) {
        poll(POLLIN, timeout);
        n = recv(fd, buffer, ...);
        if (n == 0) break;  // EOF
        response_buf->last += n;
        
        // 转发到客户端 (阻塞写)
        while (response_buf->pos < response_buf->last) {
            poll(POLLOUT, timeout);
            sent = send(client_fd, ...);
            response_buf->pos += sent;
        }
    }

    // ── 6. 清理 ──
    close(fd);
    return RPS_HTTP_DONE;
}
```

核心：`create_request` / `process_header` 两个回调**完全没变**，变的只是 I/O 方式：
| 操作 | 事件版 | 线程版 |
|------|--------|--------|
| connect | `EINPROGRESS` + write handler | `EINPROGRESS` + `poll(POLLOUT)` |
| send | write handler 回调 | `poll(POLLOUT)` + `send()` 循环 |
| recv | read handler 回调 | `poll(POLLIN)` + `recv()` 循环 |
| body 转发 | `forward_body` → `write_filter` | `poll(POLLOUT)` + `send(client)` 循环 |

## 五、WebSocket 代理：poll 双工

thread 模式下 WS handler 分两个阶段：

### 握手阶段（阻塞，同 HTTP 代理）

```
connect → send(upgrade request) → recv(101) → process_header → send(client, 101)
```

### 双向转发阶段（poll 同时监听两个 fd）

```c
static void *
ws_thread_forward(rps_http_request_t *r, rps_upstream_t *u) {
    int client_fd = r->connection->fd;
    int backend_fd = u->peer->fd;
    struct pollfd pfds[2];

    pfds[0].fd = client_fd;
    pfds[1].fd = backend_fd;

    while (1) {
        pfds[0].events = pfds[1].events = POLLIN;
        poll(pfds, 2, 3600000);  // 1h 超时

        if (pfds[0].revents & POLLIN) {   // 客户端→后端
            n = recv(client_fd, buf, ...);
            if (n <= 0) break;
            // 阻塞写后端
            while (sent < n) {
                poll((struct pollfd){{backend_fd, POLLOUT}}, 1, timeout);
                sent += send(backend_fd, ...);
            }
        }

        if (pfds[1].revents & POLLIN) {   // 后端→客户端
            n = recv(backend_fd, buf, ...);
            if (n <= 0) break;
            while (sent < n) {
                poll((struct pollfd){{client_fd, POLLOUT}}, 1, timeout);
                sent += send(client_fd, ...);
            }
        }
    }

    close(client_fd); close(backend_fd);
    // 清理 request/connection
    return NULL;
}
```

无缓冲版本（Phase 1），每侧 `poll` 读写交替。后续可加 `POLLOUT` 订阅做背压缓冲。

## 六、文件结构

建议新增一个文件，不改散现有代码：

```
src/thread/
├── rps_thread.h       # thread_ctx_t, 接口声明
├── rps_thread.c       # thread_worker(), thread_spawn()
└── rps_thread_proxy.c # thread_proxy_run(), ws_thread_forward()
```

改动现有文件：

| 文件 | 改动 |
|------|------|
| `rps_cycle.h` | 加两个 `pthread_mutex_t` |
| `rps_connection.c` | `get/free_connection` 加锁 |
| `rps_upstream.c` | `new/close_peer_conn` 加锁 |
| `rps.c` | `rps_event_accept` 加 `if_pthread` 分支 |
| `rps_http_proxy_module.c` | handler 加 `if_pthread` 分流 |
| 构建 | CMakeLists.txt 加 `pthread` 链接 + 新文件 |

## 七、测试计划

1. 用现有 `test_backend` (HTTP) 和简单 WS echo 后端
2. 配置 `if_pthread 1;` 或新增指令
3. `ab -c 10 -n 100` 并发压测
4. 对比事件版和线程版行为一致性
