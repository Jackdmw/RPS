# RPS 配置指令参考

## 顶层指令

### worker_processes
```
语法: worker_processes <number>;
默认: 1
上下文: main
```
Worker 进程数。master 进程 fork 出指定数量的 worker，每个 worker 独立运行事件循环。

### daemon
```
语法: daemon on | off;
默认: off
上下文: main
```
是否以守护进程模式运行。on 时 RPS 脱离终端，stdin/stdout/stderr 重定向到 /dev/null。

### pid
```
语法: pid <file>;
默认: run_pid.conf
上下文: main
```
PID 文件路径。启动时创建并加排他锁，防止多实例同时运行。

---

## event {} 块

```
语法: event { ... }
上下文: main
```

### use
```
语法: use epoll | io_uring | multithreading;
默认: epoll
上下文: event
```
事件驱动引擎选择。
- `epoll` — 事件驱动模式（epoll + 非阻塞 I/O）
- `io_uring` — 预留
- `multithreading` — 一连接一线程模式

### worker_connections
```
语法: worker_connections <number>;
默认: 512
上下文: event
```
每个 worker 进程的最大并发连接数。决定 `cycle->connections[]` 连接池大小。

---

## http {} 块

```
语法: http { ... }
上下文: main
```

### http_body_max_size
```
语法: http_body_max_size <size>;
默认: —
上下文: http, server, location
```
客户端请求体最大字节数。目前仅解析存储，尚未强制执行。

---

## server {} 块

```
语法: server { ... }
上下文: http
```

### listen
```
语法: listen <port>;
默认: —
上下文: server
```
监听端口。目前固定绑定 `INADDR_ANY`（所有地址）。

### server_name
```
语法: server_name <name>;
默认: —
上下文: server
```
虚拟主机名。FIND_CONFIG 阶段与 Host 头匹配。

---

## location {} 块

```
语法: location <path> { ... }
上下文: server
```
URI 路径前缀匹配。同一 server 下多个 location 按最长前缀匹配。

---

## upstream {} 块

```
语法: upstream <name> { ... }
上下文: http
```
定义后端服务器组，`proxy_pass http://<name>` 引用。

### server
```
语法: server <address> [weight=N] [max_fails=N] [fail_timeout=Ns] [down] [backup];
默认: weight=1 max_fails=1 fail_timeout=10s
上下文: upstream
```

| 参数 | 说明 |
|------|------|
| `address` | 主机:端口，如 `127.0.0.1:8080`。省略端口默认 80 |
| `weight=N` | 轮询权重，默认 1 |
| `max_fails=N` | 最大连续失败次数，超限后进入冷却，默认 1 |
| `fail_timeout=Ns` | 失败冷却时间。`10s` = 10 秒，默认 10000ms |
| `down` | 永久摘除此节点 |
| `backup` | 仅当所有非 backup 节点不可用时使用 |

### keepalive
```
语法: keepalive <number>;
默认: 0
上下文: upstream
```
后端连接空闲缓存池大小。0 表示禁用 keepalive，每次代理新建连接。大于 0 时启用连接复用。

### keepalive_timeout
```
语法: keepalive_timeout <time>;
默认: 60000ms
上下文: upstream
```
空闲后端连接超时时间（毫秒）。超时后连接被关闭回收。

### keepalive_requests
```
语法: keepalive_requests <number>;
默认: 100
上下文: upstream
```
单个后端连接最多复用的请求数。超过后连接被关闭。

---

## proxy 模块指令

所有 proxy 指令上下文为 `location`。

### proxy_pass
```
语法: proxy_pass <URL>;
默认: —
上下文: location
```

两种形式：
- `proxy_pass http://<host>:<port>/<path>` — 直接代理
- `proxy_pass http://<upstream_name>` — 引用 upstream{} 块

示例：
```nginx
proxy_pass http://127.0.0.1:8080/api;
proxy_pass http://backend;           # 引用 upstream backend {}
```

### proxy_set_header
```
语法: proxy_set_header <key> <value>;
默认: —
上下文: location
```
添加/覆盖发往后端的请求头。配置解析时预计算 key 的 djb2 hash，转发时 hash 先行加速去重。

```nginx
proxy_set_header Host $host;
proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
```

### proxy_connect_timeout
```
语法: proxy_connect_timeout <time>;
默认: 60000ms
上下文: location
```
连接后端超时（毫秒）。

### proxy_read_timeout
```
语法: proxy_read_timeout <time>;
默认: 60000ms
上下文: location
```
读取后端响应超时（毫秒）。

### proxy_send_timeout
```
语法: proxy_send_timeout <time>;
默认: 60000ms
上下文: location
```
发送请求到后端超时（毫秒）。

### proxy_buffering
```
语法: proxy_buffering on | off;
默认: on
上下文: location
```
是否缓冲后端响应。目前框架已预留，过滤链尚未实现完整缓冲。

### proxy_pass_request_headers
```
语法: proxy_pass_request_headers on | off;
默认: on
上下文: location
```
是否将客户端请求头转发给后端。on 时自动去重（跳过已由 `proxy_set_header` 覆盖 + 内置头）。

### proxy_pass_request_body
```
语法: proxy_pass_request_body on | off;
默认: on
上下文: location
```
是否将客户端请求体转发给后端。

---

## 完整配置示例

```nginx
worker_processes 4;
daemon on;
pid logs/rps.pid;

event {
    use epoll;
    worker_connections 1024;
}

http {
    upstream backend {
        server 127.0.0.1:8080 weight=3 max_fails=2 fail_timeout=10s;
        server 127.0.0.1:8081;
        keepalive 32;
        keepalive_timeout 60000;
        keepalive_requests 100;
    }

    server {
        listen 8000;
        server_name "example.com";

        location / {
            proxy_pass http://backend;
            proxy_set_header Host $host;
        }

        location /api {
            proxy_pass http://127.0.0.1:9090/api;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_connect_timeout 30000;
            proxy_read_timeout 30000;
        }

        location /ws {
            proxy_pass http://backend;
        }
    }
}
```

## 指令索引

| 指令 | 上下文 | 默认值 | 模块 |
|------|--------|--------|------|
| `worker_processes` | main | 1 | core |
| `daemon` | main | off | core |
| `pid` | main | run_pid.conf | core |
| `event { }` | main | — | event |
| `use` | event | epoll | event_core |
| `worker_connections` | event | 512 | event_core |
| `http { }` | main | — | http_module |
| `http_body_max_size` | http,server,location | — | http_core |
| `server { }` | http | — | http_core |
| `listen` | server | — | http_core |
| `server_name` | server | — | http_core |
| `location` | server | — | http_core |
| `upstream { }` | http | — | upstream |
| `server` | upstream | — | upstream |
| `keepalive` | upstream | 0 | upstream |
| `keepalive_timeout` | upstream | 60000ms | upstream |
| `keepalive_requests` | upstream | 100 | upstream |
| `proxy_pass` | location | — | http_proxy |
| `proxy_set_header` | location | — | http_proxy |
| `proxy_connect_timeout` | location | 60000ms | http_proxy |
| `proxy_read_timeout` | location | 60000ms | http_proxy |
| `proxy_send_timeout` | location | 60000ms | http_proxy |
| `proxy_buffering` | location | on | http_proxy |
| `proxy_pass_request_headers` | location | on | http_proxy |
| `proxy_pass_request_body` | location | on | http_proxy |
