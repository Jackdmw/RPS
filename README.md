# RPS — 轻量 HTTP 反向代理服务器

本项目是基于 C 语言参考 nginx 架构实现的事件驱动反向代理服务器，作为计算机网络，操作系统的实践学习项目。

## 架构概述

```
┌─────────────────────────────────────────────────────┐
│                    Master Process                    │
│  ┌─────────────────────────────────────────────────┐│
│  │ 信号管理 │ PID 文件 │ Worker 进程生命周期        ││
│  └─────────────────────────────────────────────────┘│
│                         │ fork                       │
│         ┌───────────────┼───────────────┐            │
│         ▼               ▼               ▼            │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐         │
│  │ Worker 0 │   │ Worker 1 │   │ Worker N │  ...    │
│  │ epoll    │   │ epoll    │   │ epoll    │         │
│  └──────────┘   └──────────┘   └──────────┘         │
└─────────────────────────────────────────────────────┘

请求处理流程:
  accept → 解析请求行/头 → 阶段引擎 → 代理(upstream) → 返回响应
```

## 模块系统

| 模块类型 | 模块 | 文件 | 职责 |
|----------|------|------|------|
| CORE | `core` | `rps_core_module.c` | daemon、worker_processes、pid |
| EVENT | `event_core` | `rps_event_core_module.c` | epoll 事件引擎、连接池、use/worker_connections |
| HTTP | `http_core` | `rps_http_core_module.c` | server/location/listen 解析、虚拟主机匹配、阶段引擎 |
| HTTP | `upstream` | `rps_upstream.c` | upstream{} 块、负载均衡、keepalive 连接池 |
| HTTP | `http_proxy` | `rps_http_proxy_module.c` | proxy_pass、请求构造、header 去重、响应转发 |

## 核心子系统

### 事件驱动
- epoll (ET 模式) + 非阻塞 I/O
- 定时器红黑树，毫秒级超时管理
- 连接池：`cycle->connections[]` 预分配，`free_connection` 链表复用

### 内存管理
- 分层内存池 (`rps_pool_t`)：cycle / connection / request 三级
- 小块从池内分配，大块 `malloc` 后挂入 large 链表
- 请求结束销毁 `r->pool`，所有请求级内存 (request / upstream / buf / chain) 一次性释放

### HTTP 解析与阶段引擎
- 状态机解析：请求行 (0) → 请求头 (1) → 完成 (2)
- 11 阶段管道：`POST_READ → SERVER_REWRITE → FIND_CONFIG → REWRITE → POST_REWRITE → PREACCESS → ACCESS → POST_ACCESS → PRECONTENT → CONTENT → LOG`
- 每个阶段支持多个 handler 注册，checker 控制跳转

### Upstream 与负载均衡
- `upstream {}` 块多 peer 管理
- 加权轮询 (WRR)：`weight` / `effective_weight` / `current_weight`
- 故障转移：`max_fails` + `fail_timeout` 冷却
- 连接对象 `cycle` 级空闲链表（跨 upstream 块共享）

### Keepalive 连接复用
- 配置项：`keepalive` / `keepalive_timeout` / `keepalive_requests`
- 空闲连接 LIFO 栈缓存 + 超时回调清理
- 单连接复用次数限制（`peer->upstream_requests` 计数）
- `rps_upstream_free_peer` 缓存决策：配置启用 + 后端同意 + 缓存未满

### 反向代理
- `proxy_pass http://backend[:port][/path]` 支持引用 `upstream {}` 块
- 请求构造：零拷贝直接写入 `u->request_bufs`
- Header 去重：内置头 (host/connection/x-forwarded-for) + `proxy_set_header` 覆盖
- `X-Real-IP` 多层代理透传，`X-Forwarded-For` 追加而非覆盖
- 后端 Connection 头解析，HTTP/1.1 默认 keep-alive
- 响应 body 走 HTTP 过滤链 (`write_filter`)，客户端写阻塞时背压暂停后端读
- 协议回调抽象 (`create_request` / `process_header` / `forward_body`)，WebSocket 模块可直接置换

## 配置指令

```
worker_processes 4;
daemon on;
pid logs/rps.pid;

events {
    use epoll;
    worker_connections 1024;
}

http {
    upstream backend {
        server 127.0.0.1:8080 weight=3 max_fails=2 fail_timeout=10s;
        server 127.0.0.1:8081;
        keepalive 32;
        keepalive_timeout 60s;
        keepalive_requests 100;
    }

    server {
        listen 8080;

        location / {
            proxy_pass http://backend;
            proxy_set_header Host $host;
        }
    }
}
```

## 构建与运行

```sh
cd build && cmake .. && make

# 启动
./RPS -c default.conf

# 测试后端 (keep-alive 支持)
./test_backend 9090
```

测试后端输出格式：
```
new connection (total: 1)
[GET] / → 100 bytes  (keep-alive)
[close] fd=5  reason=idle_timeout  alive=35s  reqs=2
[close] fd=6  reason=connection_reset  alive=2s  reqs=1  errno=104(Connection reset by peer)
```

## 项目结构

```
src/
├── core/           # 基础库：pool、buf、list、array、rbtree、connection、cycle、配置解析
├── event/          # 事件驱动：epoll 引擎、定时器
│   └── modules/
├── http/           # HTTP 层：解析、阶段引擎、响应、upstream
│   └── modules/    # HTTP 模块：http_core、http_proxy、upstream
└── test/           # 测试后端
```

## TODO


- [ ] 动态 peer 管理 (DNS 解析刷新)
