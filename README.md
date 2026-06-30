# RPS — 轻量级 HTTP 反向代理服务器

基于 C 语言参考 nginx 架构实现的事件驱动反向代理服务器，支持 Reactor（epoll）和多线程两种执行模型，含 WebSocket 代理。
作为本学期操作系统，计算机网络的实践项目

## 项目概况

- **语言：** C11 + POSIX
- **构建：** CMake ≥ 3.10
- **规模：** 51 个源文件，~10,000 行代码
- **依赖：** Linux、epoll、pthread

## 功能特性

- **HTTP/1.1 反向代理** — 请求解析、头部透传、响应转发、keepalive
- **WebSocket 代理** — 101 升级握手 + 全双工双向转发 + 反压缓冲
- **负载均衡** — 平滑加权轮询（WRR），故障转移 + backup 回退
- **连接复用** — 后端 keepalive 连接池（LIFO 栈），可配置超时和复用次数
- **双执行模型** — 一行配置切换 `use epoll`（事件驱动）或 `use multithreading`（线程模式）
- **模块化架构** — 声明式配置解析，新功能通过模块注册添加

## 架构概述

```
┌──────────────────────────────────────────────────────┐
│                    Master Process                     │
│  信号管理 │ PID 文件锁 │ Worker 生命周期              │
└────────────────────────┬─────────────────────────────┘
                         │ fork
         ┌───────────────┼───────────────┐
         ▼               ▼               ▼
   Worker 0         Worker 1         Worker N
   epoll 事件循环    epoll 事件循环    epoll 事件循环
   (或线程模式)      (或线程模式)      (或线程模式)
                         │
              SO_REUSEPORT 内核分发
                         │
              accept → 解析 → 阶段引擎 → 代理 → 响应
```

## 两种执行模型

通过 `use` 指令切换，两种模式共享全部业务逻辑（解析器、阶段引擎、代理、负载均衡），只在 I/O 层有差异：

| | Reactor (epoll) | Thread (multithreading) |
|---|---|---|
| I/O 模型 | 非阻塞 + epoll ET | 阻塞 + poll |
| 并发方式 | 单线程事件循环 | 每连接一个 detached pthread |
| 状态驱动 | 事件回调函数指针替换 | 同步代码顺序执行 |
| 超时管理 | 红黑树统一管理 | poll() timeout |
| 线程安全 | 不需要（单线程） | mutex 条件加锁 |

## 模块系统

8 个内置模块，按类型分三类：

| 模块 | 类型 | 职责 |
|------|------|------|
| `core` | CORE | daemon、worker_processes、pid |
| `event` | CORE | event {} 块解析 |
| `event_core` | EVENT | use/worker_connections 指令、连接池初始化 |
| `epoll` | EVENT | epoll 事件引擎实现 |
| `http` | CORE | http {} 块解析 |
| `http_core` | HTTP | server/location/listen、虚拟主机匹配、阶段引擎 |
| `upstream` | HTTP | upstream {} 块、WRR 负载均衡、keepalive 连接池 |
| `http_proxy` | HTTP | proxy_pass、HTTP/WS 代理 |

## 核心子系统

### 内存管理

- 分层内存池：Cycle Pool → Connection Pool → Request Pool
- 小块 bump 指针分配 O(1)，大块 malloc + 链表追踪
- 请求结束整池销毁，不逐块释放——比 GC 快

### 事件驱动

- epoll 边沿触发（ET），非阻塞 I/O
- 定时器红黑树统一管理所有超时，epoll_wait 超时由树的最小到期时间决定
- 连接池预分配，freelist 链表复用
- Handler 函数指针替换驱动状态机

### HTTP 解析与阶段引擎

- 状态机解析：请求行 (0) → 请求头 (1) → 完成 (2)
- 常用头部（Host、Content-Length、Connection、Upgrade 等）落入专用字段，其余存链表透传
- 11 阶段管道：`POST_READ → SERVER_REWRITE → FIND_CONFIG → REWRITE → POST_REWRITE → PREACCESS → ACCESS → POST_ACCESS → PRECONTENT → CONTENT → LOG`
- 每个 handler 含 checker（流程控制）+ handler（业务逻辑）+ next（跳转目标）
- 关键阶段：FIND_CONFIG 做虚拟主机匹配，REWRITE 支持 URL 重写后重新路由，CONTENT 为代理入口

### 响应输出

- Chain Buffer 单向链表串联响应头 + body
- header_filter 自动补全 Content-Type/Length
- write_filter 非阻塞 send，EAGAIN 时注册写事件恢复，代理路径形成自动反压

### Upstream 与负载均衡

- 平滑加权轮询（WRR）：current_weight 动态调整，分配平滑均匀
- 故障转移：fails ≥ max_fails → 冷却 fail_timeout，冷却期自动跳过
- backup 节点：主节点全部不可用时 fallback
- 连接池：LIFO 栈缓存空闲连接，空闲超时自动清理

### Keepalive 连接复用

- 配置项：`keepalive`、`keepalive_timeout`、`keepalive_requests`
- 缓存连接注册 EPOLLIN 检测后端主动关闭
- 归还决策：配置启用 + 后端同意 + 缓存未满

### 反向代理

- `proxy_pass http://backend[:port][/path]`，支持引用 `upstream {}` 块
- X-Real-IP 多层代理透传，X-Forwarded-For 追加而非覆盖
- 头部去重：djb2 哈希快速判断 proxy_set_header 覆盖和内置头跳过
- 后端响应解析后通过 HTTP 过滤链发送，客户端阻塞时暂停后端读（反压）

### WebSocket 代理

- 自动检测 Upgrade: websocket + Connection: upgrade
- 透传升级请求和 101 响应
- 升级后重新绑定事件 handler，bypass HTTP 过滤链进入全双工转发
- 反压缓冲 + shutdown 半关闭检测

## 配置示例

```nginx
worker_processes 4;
daemon on;
pid run_pid.conf;

event {
    use epoll;                    # epoll / multithreading
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
        server_name example.com;

        location / {
            proxy_pass http://backend;
            proxy_set_header Host $host;
            proxy_set_header X-Real-IP $remote_addr;
            proxy_connect_timeout 30000;
            proxy_read_timeout 60000;
        }
    }
}
```

## 构建与运行

```sh
# 编译
cd build && cmake .. && make

# 前台调试运行
./RPS -c default.conf -l 8

# daemon 模式
./RPS -c /etc/rps/rps.conf

# 测试配置
./RPS -c rps.conf -t

# 停止运行中的 daemon
./RPS -c rps.conf -s stop

# 测试后端
cd ../test && python3 backend.py &    # FastAPI 测试后端 (:9090)
# 或
./test_backend 9090                   # C 语言测试后端
```

命令行参数：

| 参数 | 说明 |
|------|------|
| `-c <file>` | 配置文件路径（必需） |
| `-l <0-8>` | 日志级别，0=STDERR，8=DEBUG |
| `-p <path>` | 安装前缀 |
| `-t` | 测试配置语法后退出 |
| `-s stop` | 停止 daemon |

## 项目结构

```
RPS/
├── src/
│   ├── core/          # 基础设施：内存池、字符串、容器、日志、配置、模块、进程
│   ├── event/         # 事件引擎：epoll、定时器、连接池
│   │   └── modules/
│   ├── http/          # HTTP 协议：解析、阶段引擎、响应、upstream
│   │   └── modules/   # HTTP 模块：http_core、http_proxy
│   └── thread/        # 多线程模式：线程 I/O、worker、proxy
├── test/              # 测试后端和集成测试脚本
├── config/            # 配置语法文档
├── docs/              # 项目文档
└── CMakeLists.txt
```

## TODO

- [ ] 动态 peer 管理（DNS 解析刷新）
- [ ] io_uring 事件引擎
- [ ] SSL/TLS 支持
