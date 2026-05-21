# RPS

用 C 语言实现的类 Nginx 架构的反向代理 / Web 服务器，作为学习 Nginx 核心架构的实践项目。

## 对照 Nginx 的实现状态

### 基础库 (core/)

| 组件 | 状态 | 对应 Nginx | 说明 |
|---|---|---|---|
| 内存池 `rps_palloc` | 完成 | `ngx_palloc` | 小块+大块分配，4次失败跳过策略 |
| 字符串 `rps_string` | 完成 | `ngx_string` | 带长度的字符串，比较宏 |
| 动态数组 `rps_array` | 完成 | `ngx_array` | 支持原地扩容，2倍增长 |
| 块链表 `rps_list` | 完成 | `ngx_list` | 连续块+链表，适合大量小对象 |
| 红黑树 `rps_rbtree` | 完成 | `ngx_rbtree` | 含插入/删除/再平衡/交换 |
| 缓冲区 `rps_buf` | 完成 | `ngx_buf` | 支持覆盖/不覆盖两种读取模式 |
| 日志 `rps_log` | 完成 | `ngx_log` | 8级日志，支持errno描述 |
| 文件抽象 `rps_file` | 完成 | `ngx_file` | 封装 fd + name + stat |

### 模块系统

| 组件 | 状态 | 说明 |
|---|---|---|
| 模块框架 | 完成 | `RPS_CORE_MODULE` / `RPS_EVENT_MODULE` / `RPS_HTTP_MODULE` 三级 |
| 模块索引 | 完成 | `rps_preinit_modules` 分配 index/ctx_index |
| core 模块 | 完成 | `worker_processes`, `daemon`, `pid` 指令 |
| event_core 模块 | 完成 | `use` (epoll/io_uring/thread_block), `worker_connections` |
| http 模块 | **仅头文件** | `rps_http_conf_container_t` 定义了 main/srv/loc 三级配置容器 |

### 配置解析

| 组件 | 状态 | 说明 |
|---|---|---|
| 词法解析器 | 完成 | 识别 单词/引号字符串/注释/分号/花括号 |
| 语法解析器 | 完成 | 指令查找、参数个数校验、块层级校验 |
| 指令分发 | 完成 | 5 种 conf 归属 (CORE/EVENT/HTTP_MAIN/SRV/LOC) |
| set 函数 | 完成 | flag_slot, str_slot, num_slot 三类通用 setter |

### 进程模型

| 组件 | 状态 | 说明 |
|---|---|---|
| Master-Worker 架构 | 完成 | fork workers, 信号管理 |
| Daemon 模式 | 完成 | 双 fork + setsid |
| 信号处理 | 基础 | SIGINT/SIGQUIT/SIGCHLD，缺 SIGHUP reload |
| Worker 进程循环 | **骨架** | 仅有空循环，event_type=1 时有 accept 调用(未完成) |

### 事件子系统

| 组件 | 状态 | 说明 |
|---|---|---|
| 事件数据结构 | 完成 | `rps_event_t` (handler/data/flags/timer) |
| 事件模块框架 | 完成 | create_conf / init_conf 钩子 |
| epoll 初始化 | **未实现** | `epoll_create` / `epoll_ctl` / `epoll_wait` |
| io_uring 初始化 | **未实现** | 仅配置解析支持 |
| 事件循环 | **未实现** | 核心的 `rps_process_events` 待写 |
| 定时器 | **未实现** | 红黑树已有，但超时检测逻辑未写 |

### 连接管理

| 组件 | 状态 | 说明 |
|---|---|---|
| 监听端口 `rps_listening_t` | 完成 | socket/bind/listen 已实现 |
| 连接池 `connections` | 部分 | `rps_event_core_init_process` 中创建，但有bug(赋值当比较) |
| accept 获取连接 | 完成 | `rps_get_connection` |
| 非阻塞设置 | **未实现** | 函数声明了但未实现 body |
| Unix send/recv | **未实现** | 声明了但未实现 body |

### HTTP 子系统

| 组件 | 状态 | 说明 |
|---|---|---|
| HTTP 模块 | **未实现** | 无 .c 文件 |
| 请求解析 | **未实现** | 请求行/请求头/body |
| 响应生成 | **未实现** | 状态行/响应头/body |
| 虚拟主机(server) | **未实现** | 配置框架预留了 main/srv/loc |
| Location 匹配 | **未实现** | |
| MIME 类型 | **未实现** | |
| Keep-Alive | **未实现** | |

### 反向代理

| 组件 | 状态 | 说明 |
|---|---|---|
| Upstream | **未实现** | |
| Proxy Pass | **未实现** | |

---

## 当前架构概览

```
main()
 ├── rps_log_init()              # 日志初始化
 ├── parse_cmd()                 # 解析 -c 命令行参数
 ├── rps_preinit_modules()       # 模块编号分配
 └── rps_init_cycle()
      ├── rps_create_pool()      # 创建cycle内存池
      ├── create_conf()          # 各核心模块创建默认配置
      ├── rps_conf_parse()       # 解析配置文件
      │    ├── rps_conf_read_token()   # 词法分析
      │    └── rps_conf_handler()      # 语法分析+指令分发
      ├── init_conf()            # 配置默认值填充 (新增)
      └── rps_master_process_cycle()
           ├── rps_daemon()      # 可选的守护进程化
           ├── fork() x N        # 创建worker子进程
           ├── sigsuspend()      # 等待信号
           └── rps_worker_process_cycle()
                ├── rps_worker_process_init()  # 调用模块init_process
                └── while(1) {}               # 空事件循环 (待实现)
```

## 配置文件语法 (rps.conf)

```nginx
daemon off;
pid logs/rps.pid;
worker_processes 4;

event {
    use epoll;
    worker_connections 1024;
}

# HTTP 块（解析框架已预留，模块本身未实现）
# http {
#     server {
#         listen 80;
#         server_name example.com;
#         location / {
#             proxy_pass http://backend;
#         }
#     }
# }
```

## 下一步开发路线

### 第一阶段：让事件循环跑起来

这是当前最关键的缺失部分，直接阻塞了所有上层功能。

1. **实现 epoll 事件模块** — 新建 `src/event/modules/rps_epoll_module.c`
   - `epoll_create` 初始化
   - `rps_event_add` / `rps_event_del` 注册/删除事件
   - `rps_process_events` 主循环：`epoll_wait` → 分发到 `ev->handler`
2. **补全非阻塞工具** — `rps_set_nonblocking()` 和 `rps_unix_recv/send()`
3. **连接池与事件绑定** — `rps_get_connection` 时为 fd 创建 read/write event，注册到 epoll
4. **定时器** — 在事件循环中检查红黑树最小节点，计算 `epoll_wait` 超时时间

### 第二阶段：HTTP 模块基础

5. **实现 HTTP 核心模块** — `src/http/rps_http_core.c`
   - `listen` 指令 set 函数
   - `server_name`, `location` 块指令
6. **HTTP 请求解析** — `src/http/rps_http_parse.c`
   - 请求行 (`GET / HTTP/1.1`)
   - 请求头解析
7. **HTTP 响应生成** — 静态文件返回、基本状态码

### 第三阶段：反向代理

8. **Upstream 模块** — 后端连接池、健康检查
9. **Proxy Pass** — 转发请求到 upstream，透传响应

---

## 已知 Bug

| 位置 | 问题 |
|---|---|
| `rps_event_core.c:101` | `if (cycle -> connections = NULL)` → 赋值当比较，应为 `==` |
| `rps_connection.c:64` | `if (conn = NULL)` → 同上 |
| `rps_event_module.c:73-74` | `rps_event_init_conf` 循环边界使用 `rps_event_modules_n`，与 `rps_event_create_conf` 不一致 |
| `rps_core_module.c:146` | `rps_core_init_conf` return 类型为 `void *` 但函数无 return 语句 |
| `rps_rbtree.h:51` vs `rps_rbtree.c:170` | `rps_rbtree_next` 声明为2参数，实现为3参数（多了 root） |

## HTTP 模块架构设计

Nginx 的 HTTP 子系统采用**模块化 + 多级配置 + 阶段化处理**的架构。下面对照 Nginx 原生设计，说明 RPS 中 HTTP 模块的结构、数据流，以及实现基础代理转发需要的最小模块集合。

### 1. 模块上下文接口

在 Nginx 中，每个 HTTP 模块通过 `ngx_http_module_t` 提供配置创建/合并钩子。RPS 中对应的接口：

```c
// 对标 ngx_http_module_t，定义在 rps_module.h 或 http 子系统中
typedef struct {
    // 配置解析前后
    rps_int_t (*preconfiguration)(rps_conf_t *cf);
    rps_int_t (*postconfiguration)(rps_conf_t *cf);

    // 三级配置的创建（在解析 http{} 块之前调用）
    void *(*create_main_conf)(rps_conf_t *cf);    // http {} 级
    void *(*create_srv_conf)(rps_conf_t *cf);     // server {} 级
    void *(*create_loc_conf)(rps_conf_t *cf);     // location {} 级

    // 配置合并（上级 → 下级，实现继承）
    char *(*merge_srv_conf)(rps_pool_t *pool, void *parent, void *child);
    char *(*merge_loc_conf)(rps_pool_t *pool, void *parent, void *child);
} rps_http_module_ctx_t;
```

**配置继承关系：**

```
http {} 级配置 (main_conf)
  │  通过 merge_srv_conf 合并
  └── server {} 级配置 (srv_conf)
        │  通过 merge_loc_conf 合并
        └── location {} 级配置 (loc_conf)
```

- `create_*_conf` 返回该级的默认值（用 `RPS_CONF_UNSET_*` 填充）
- `merge_*_conf` 把 parent 的值填入 child 中那些还是 UNSET 的字段
- 模块不需要某级配置时，对应的 create/merge 留 NULL

### 2. 请求处理的 11 个阶段

Nginx 把每个 HTTP 请求的处理拆成 11 个阶段，每个阶段可注册多个 handler，按顺序执行：

| 阶段 | 作用 | 代理场景的必要性 |
|---|---|---|
| `POST_READ` | 刚读完请求头后 | 低 — 一般不挂 handler |
| `SERVER_REWRITE` | server 级 URL 重写 | 低 — 简单代理不需要 |
| `FIND_CONFIG` | 根据 URI 匹配 location | **高** — 内置阶段，决定用哪个 loc 配置 |
| `REWRITE` | location 级 URL 重写 | 低 |
| `POST_REWRITE` | 如果 URI 变了，重新匹配 location | 低 |
| `PREACCESS` | 访问控制预处理（如限流） | 中 |
| `ACCESS` | 访问控制（allow/deny） | 中 |
| `POST_ACCESS` | 访问被拒时的处理 | 中 |
| `PRECONTENT` | 内容生成前的最后检查（try_files） | 低 |
| `CONTENT` | **实际生成响应内容** | **高** — proxy 模块在此挂 handler |
| `LOG` | 记录访问日志 | 高 — 日志模块在此挂 handler |

```c
// 阶段定义
typedef enum {
    RPS_HTTP_POST_READ_PHASE = 0,
    RPS_HTTP_SERVER_REWRITE_PHASE,
    RPS_HTTP_FIND_CONFIG_PHASE,
    RPS_HTTP_REWRITE_PHASE,
    RPS_HTTP_POST_REWRITE_PHASE,
    RPS_HTTP_PREACCESS_PHASE,
    RPS_HTTP_ACCESS_PHASE,
    RPS_HTTP_POST_ACCESS_PHASE,
    RPS_HTTP_PRECONTENT_PHASE,
    RPS_HTTP_CONTENT_PHASE,
    RPS_HTTP_LOG_PHASE,
} rps_http_phases;

// handler 注册
typedef rps_int_t (*rps_http_handler_pt)(rps_http_request_t *r);

// 每个阶段是一个 checker 数组
typedef struct {
    rps_http_handler_pt  *handlers;   // 按顺序执行
    rps_uint_t            n;
} rps_http_phase_t;
```

**阶段执行规则：**
- handler 返回 `RPS_OK` → 继续本阶段的下一个 handler
- handler 返回 `RPS_DECLINED` → 继续本阶段的下一个 handler
- handler 返回 `RPS_AGAIN` → 暂停（等数据到了再重入）
- handler 返回 `RPS_ERROR` → 中断，500
- **CONTENT 阶段特殊**：返回 `RPS_OK` 表示已生成内容，不再调用后续 handler

### 3. 核心数据结构

```c
// HTTP 请求 — 贯穿整个处理流程的核心对象
typedef struct rps_http_request_s {
    rps_connection_t       *connection;    // 关联的连接

    // 请求信息
    rps_str_t               method;        // GET / POST / ...
    rps_str_t               uri;
    rps_str_t               args;          // query string
    rps_str_t               http_version;  // HTTP/1.0 或 HTTP/1.1
    rps_str_t               host;          // Host 请求头

    // 请求头 / 响应头
    rps_http_headers_in_t   headers_in;
    rps_http_headers_out_t  headers_out;

    // 请求体 / 响应体（使用 buffer 链）
    rps_buf_t              *request_body;
    rps_chain_t            *out_chain;     // 响应 buffer 链

    // 匹配到的 location 配置
    void                   **loc_conf;     // 指向匹配到的 loc_conf 数组

    // 状态
    rps_uint_t               phase;        // 当前阶段
    rps_uint_t               phase_index;  // 阶段内 handler 下标

    rps_pool_t              *pool;         // 请求专属内存池
    rps_log_t               *log;
} rps_http_request_t;

// Buffer 链 — 用于构造响应数据（多个 buf 拼成一次 send）
typedef struct rps_chain_s {
    rps_buf_t              *buf;
    struct rps_chain_s     *next;
} rps_chain_t;

// 请求头解析结果（简化版）
typedef struct {
    rps_str_t   host;
    rps_str_t   user_agent;
    rps_str_t   content_type;
    rps_str_t   content_length;
    rps_str_t   connection;       // keep-alive / close
    size_t      content_length_n; // 解析后的数字
} rps_http_headers_in_t;

// 响应头发送前构造
typedef struct {
    rps_uint_t  status;
    rps_str_t   content_type;
    rps_str_t   server;          // "RPS"
    size_t      content_length_n;
} rps_http_headers_out_t;
```

### 4. 请求处理主流程

```
rps_http_process_request(r)
 ├── rps_http_parse_request_line(r)    # "GET / HTTP/1.1\r\n"
 ├── rps_http_parse_headers(r)         # "Host: ...\r\n..."
 └── rps_http_phase_engine(r)          # 逐阶段执行
      ├── POST_READ       → 各模块注册的 handler
      ├── FIND_CONFIG     → 匹配 location，设置 r->loc_conf
      ├── ...
      ├── CONTENT         → rps_proxy_handler(r)  ← proxy 模块在此
      └── LOG             → 写 access log
 └── rps_http_finalize_request(r)      # 发送响应、决定是否 keep-alive
```

### 5. 实现基础代理转发需要的最小模块

```
                     ┌─────────────────┐
                     │ rps_http_request │  ← 请求/响应对象 + 解析器
                     │ (基础设施，非模块) │
                     └────────┬────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌───────────────┐  ┌──────────────────┐  ┌───────────────────┐
│ http_core     │  │ http_upstream    │  │ http_proxy        │
│ (HTTP核心模块)  │  │ (上游服务器管理)    │  │ (代理转发模块)      │
└───────────────┘  └──────────────────┘  └───────────────────┘
│ 提供：           │  │ 提供：             │  │ 提供：              │
│ • http{} 块     │  │ • upstream{} 块   │  │ • proxy_pass      │
│ • listen 指令    │  │ • server 指令     │  │ • 转发请求到upstream│
│ • server_name   │  │ • 后端连接池       │  │ • 返回响应给客户端   │
│ • location 指令  │  │ • 健康检查         │  │ • 代理请求头处理     │
│ • 全局location匹配│  │ • 负载均衡(最少轮询) │  │                    │
│ • 阶段引擎驱动   │  │                   │  │                    │
└───────────────┘  └──────────────────┘  └───────────────────┘
```

#### 模块 A：`rps_http_request`（基础设施，不是模块）

**职责：** 解析 HTTP 请求、管理请求/响应生命周期、驱动处理阶段

**头文件声明：**

```c
// 创建、销毁
rps_http_request_t *rps_http_create_request(rps_connection_t *c);
void rps_http_close_request(rps_http_request_t *r);

// 解析
rps_int_t rps_http_parse_request_line(rps_http_request_t *r);
rps_int_t rps_http_parse_headers(rps_http_request_t *r);

// 阶段引擎
rps_int_t rps_http_phase_engine(rps_http_request_t *r);

// 发送响应
rps_int_t rps_http_send_header(rps_http_request_t *r);
rps_int_t rps_http_send_body(rps_http_request_t *r, rps_buf_t *body);
rps_int_t rps_http_output_filter(rps_http_request_t *r, rps_chain_t *out);

// 注册阶段 handler（由 http_core 的 postconfiguration 调用）
void rps_http_register_phase_handler(rps_uint_t phase, rps_http_handler_pt handler);

// 访问通用响应状态页（404, 500...）
void rps_http_finalize_request(rps_http_request_t *r, rps_uint_t status);
```

**关键实现细节：**

- 请求行解析是一个**状态机**（因为 TCP 是流的，一次 `read` 可能只拿到半行）
- 请求头解析同理，需要逐字符扫描 `\r\n`
- Buffer 链 (`rps_chain_t`) 用于构造响应 — 响应行、响应头、响应体可以各自是一个 buf，通过链表串起来一次或分次 send
- 阶段引擎就是双重循环：外层按 phase 升序走，内层逐个调用该 phase 注册的 handler

#### 模块 B：`rps_http_core_module`

**类型：** `RPS_HTTP_MODULE`（不是 RPS_CORE_MODULE）

**职责：** HTTP 子系统的大管家 — 解析 http/server/location 层级配置、提供核心指令、驱动阶段引擎

**提供的指令：**

| 指令 | 出现位置 | 说明 |
|---|---|---|
| `http { }` | main 层（根） | http 块，启动时解析，创建所有 HTTP 模块的 main/srv/loc_conf |
| `server { }` | http 块内 | 虚拟主机，可定义多个 |
| `listen` | server 块内 | 监听端口，如 `listen 80;` — 解析后创建 `rps_listening_t` |
| `server_name` | server 块内 | 主机名匹配，如 `server_name example.com;` |
| `location` | server 块内 | URI 路径匹配，如 `location /api/ { }` |

**配置结构体（各模块各自定义，这里以 core 为例）：**

```c
// http {} 级配置
typedef struct {
    rps_array_t   servers;       // rps_http_core_srv_conf_t *
    rps_uint_t    client_max_body_size;
} rps_http_core_main_conf_t;

// server {} 级配置
typedef struct {
    rps_str_t      server_name;
    rps_uint_t     port;
    /* listen 创建的 listening，由 postconfiguration 阶段处理 */
} rps_http_core_srv_conf_t;

// location {} 级配置
typedef struct {
    rps_str_t      pattern;      // location 后面的路径，如 /api/
    rps_uint_t     exact_match:1;// = /pattern 精确匹配
} rps_http_core_loc_conf_t;
```

**关键职责：**

1. **`http {}` 块解析入口** — 遇到 `http {` 时，创建所有 HTTP 模块的三级配置容器，然后递归解析内部的 server/location 块
2. **Location 匹配** — FIND_CONFIG 阶段中，用请求 URI 去匹配 server 下的所有 location，把匹配到的 `loc_conf` 数组绑定到 request 上
3. **创建 listening 套接字** — postconfiguration 中，遍历所有 server 的 `listen` 配置，创建 `rps_listening_t` 并加入 `cycle->listening`
4. **阶段引擎初始化** — 在每个 server/location 解析完成后，把各模块通过 `postconfiguration` 注册的 handler 合并进阶段数组

**配置解析流程：**

```
http {
    server {                              ← create_srv_conf() 8次
        server_name  example.com;         ← rps_http_core 的 set 函数
        listen       80;                  ← 同上，创建 rps_listening_t
        location /api/ {                  ← create_loc_conf()
            proxy_pass http://backend:8080; ← rps_http_proxy 的 set 函数
        }
    }
}
```

#### 模块 C：`rps_http_upstream_module`

**类型：** `RPS_HTTP_MODULE`

**职责：** 管理后端服务器列表和连接

**提供的指令：**

| 指令 | 出现位置 | 说明 |
|---|---|---|
| `upstream { }` | http 块内 | 定义一个后端集群 |
| `server` | upstream 块内 | 集群中的一台机器，如 `server 10.0.0.1:8080;` |

**配置和运行时结构：**

```c
// upstream 集群
typedef struct rps_upstream_s {
    rps_str_t               name;         // 集群名称
    rps_array_t             peers;        // rps_upstream_peer_t 数组
    rps_uint_t              cur_peer;     // 轮询下标
} rps_upstream_t;

// 集群中的一个后端
typedef struct {
    rps_str_t               addr;         // IP:port
    struct sockaddr_in      sockaddr;     // 解析后的地址
    rps_uint_t              weight;       // 权重
    rps_uint_t              failures;     // 失败次数
} rps_upstream_peer_t;

// 与后端的连接（运行时）
typedef struct {
    rps_connection_t        connection;   // 到后端的 TCP 连接
    rps_upstream_peer_t    *peer;         // 指向哪个后端
    rps_pool_t             *pool;
} rps_upstream_connection_t;
```

**最少需要提供的接口：**

```c
// 查找/创建 upstream
rps_upstream_t *rps_upstream_get(rps_cycle_t *cycle, rps_str_t *name);

// 从集群中取下一个 peer（轮询）
rps_upstream_peer_t *rps_upstream_get_peer(rps_upstream_t *us);

// 获取到后端的连接（创建或从连接池取）
rps_upstream_connection_t *rps_upstream_get_connection(rps_upstream_peer_t *peer);

// 归还连接、标记失败
void rps_upstream_free_connection(rps_upstream_connection_t *uc);
void rps_upstream_peer_fail(rps_upstream_peer_t *peer);
```

**简化方案：** 如果追求最小实现，可以从简：
- 先不做 `upstream {}` 块 — `proxy_pass` 直接解析一个 IP:port 字符串
- 不用连接池 — 每次代理请求新建一个到后端的 TCP 连接
- 不用健康检查 — 连接失败直接返回 502

#### 模块 D：`rps_http_proxy_module`

**类型：** `RPS_HTTP_MODULE`

**职责：** 在 CONTENT 阶段挂 handler，把客户端请求转发给后端，把响应送回客户端

**提供的指令：**

| 指令 | 出现位置 | 说明 |
|---|---|---|
| `proxy_pass` | location 块内 | 指定转发目标，如 `proxy_pass http://10.0.0.1:8080;` |

**handler 的核心流程：**

```c
rps_int_t rps_proxy_handler(rps_http_request_t *r) {
    // 1. 拿到 location 级配置
    rps_http_proxy_loc_conf_t *plcf = rps_http_get_loc_conf(r, rps_http_proxy_module);

    // 2. 解析或查找后端地址
    rps_str_t *backend = &plcf->proxy_pass;  // "10.0.0.1:8080"

    // 3. 建立到后端的连接
    int backend_fd = socket(...);
    connect(backend_fd, ...);

    // 4. 构造代理请求发送给后端
    rps_proxy_send_request(backend_fd, r);   // 转发 method + uri + headers

    // 5. 从后端读取完整响应
    rps_proxy_read_response(backend_fd, r);  // 读到 buffer 链

    // 6. 把响应写回客户端
    rps_http_send_body(r, response_body);

    // 7. 清理
    close(backend_fd);
    rps_http_finalize_request(r, 200);
    return RPS_OK;
}
```

**线程模式下，第 4-6 步可以直接阻塞 read/write**，不需要事件驱动。这恰好印证了之前的结论：HTTP 处理逻辑是两个模型共享的，proxy_module 的 handler 不需要知道底层是线程还是 epoll。

### 6. 三级配置的存储与合并

上面 4 个模块各自有 main/srv/loc 三级配置。这部分说明它们如何组织存储、如何继承合并、以及请求时如何访问。

#### 6.1 不是每个模块都有 server 数组

只有 **`rps_http_core_module` 的 main_conf** 里有一个 `servers` 数组，用来存所有虚拟主机的 `rps_http_conf_container_t`：

```c
// 只有 core 模块的 main_conf 有 servers 数组
typedef struct {
    rps_array_t   servers;        // 每个元素是 rps_http_conf_container_t *
    // ...
} rps_http_core_main_conf_t;

// 其他模块（proxy、upstream 等）的 main_conf 只放自己关心的 http 级全局设置
typedef struct {
    rps_str_t     proxy_temp_path;   // 例如 proxy 模块存临时目录
    // 没有 servers 数组！
} rps_http_proxy_main_conf_t;
```

#### 6.2 配置上下文容器

每个 http/server/loc 级别各有一个 `rps_http_conf_container_t`，里面的三个 `void **` 覆盖所有模块的配置指针数组：

```
                           ┌──────────────────────────────┐
http 级 ctx                │ main_conf[0..N] → 每个模块的  │
(全局唯一一份)               │ http 级配置                   │
                           │                              │
                           │ srv_conf = NULL  (http 级    │
                           │ 没有 srv_conf)               │
                           └──────────────────────────────┘

                           ┌──────────────────────────────┐
server 级 ctx              │ main_conf ──→ 指向 http 级     │
(每个 server 一份)          │             main_conf (指针共享)│
                           │                              │
                           │ srv_conf[0..N] → 每个模块的    │
                           │ server 级配置                  │
                           └──────────────────────────────┘

                           ┌──────────────────────────────┐
location 级 ctx            │ main_conf ──→ 指向 http 级     │
(每个 location 一份)        │             main_conf (指针共享)│
                           │                              │
                           │ srv_conf ──→ 指向父 server 的  │
                           │             srv_conf (指针共享) │
                           │                              │
                           │ loc_conf[0..N] → 每个模块的    │
                           │ location 级配置                │
                           └──────────────────────────────┘
```

关键设计：**子级通过指针引用父级，不拷贝数组**。

#### 6.3 解析阶段 — 只创建，不合并

```
http {
    // 1. 遇到 http { → 为每个 HTTP 模块调用 create_main_conf()
    //    生成 http_ctx.main_conf[0..N]
    //    同时初始化 servers 数组（存在 core 模块的 main_conf 中）

    server {          // 2. 遇到 server { → 为每个模块调 create_srv_conf()
        listen 80;    //    生成 srv_conf[0..N]
        ...           //    srv_ctx.main_conf = http_ctx.main_conf (指针)
    }                 //    解析结束后把 srv_ctx append 到 servers 数组

    server {          // 3. 另一个 server，同上
        listen 8080;
        location / {  // 4. 遇到 location { → 为每个模块调 create_loc_conf()
            ...       //    生成 loc_conf[0..N]
        }             //    loc_ctx.srv_conf = srv_ctx.srv_conf (指针)
    }
}
```

此时每个模块的三级配置里，大量字段还是 `RPS_CONF_UNSET_*` — 值只存在于指令实际出现的那一级。

#### 6.4 合并阶段 — postconfiguration 中执行

全部解析完后，遍历每个 server 的每个 location，逐模块调用 merge 函数：

```c
// postconfiguration 中的逐级合并
for (s = 0; s < core_main_conf->servers.nelts; s++) {
    ctx = servers.elts[s];  // 这个 server 的 rps_http_conf_container_t

    // 1. http → server 合并 (调用各模块的 merge_srv_conf)
    for (m = 0; m < http_modules_n; m++) {
        module = http_modules[m];
        if (module->merge_srv_conf) {
            module->merge_srv_conf(pool,
                http_ctx.main_conf[m],  // parent
                ctx->srv_conf[m]);      // child (UNSET 字段被 parent 覆盖)
        }
    }

    // 2. server → location 合并 (遍历该 server 下的所有 location)
    for (l = 0; l < locations.nelts; l++) {
        for (m = 0; m < http_modules_n; m++) {
            if (module->merge_loc_conf) {
                module->merge_loc_conf(pool,
                    ctx->srv_conf[m],       // parent
                    loc_ctx->loc_conf[m]);  // child
            }
        }
    }
}
```

**merge 函数的逻辑极其简单：parent 有值而 child 是 UNSET → 填入 parent 的值：**

```c
// 以 proxy 模块的 merge_loc_conf 为例
char *merge_loc_conf(rps_pool_t *pool, void *parent, void *child) {
    rps_http_proxy_loc_conf_t *prev = parent;
    rps_http_proxy_loc_conf_t *conf = child;

    rps_conf_merge_uint_value(conf->connect_timeout, prev->connect_timeout, 60);
    rps_conf_merge_str_value(conf->proxy_host,     prev->proxy_host,     "$host");
    rps_conf_merge_uint_value(conf->read_timeout,   prev->read_timeout,   60);

    return RPS_CONF_OK;
}
```

每个模块对自己的配置结构体心知肚明，所以 merge 函数由模块自己提供。

#### 6.5 请求时如何拿到合并后的配置

FIND_CONFIG 阶段匹配到 server + location 后，设置请求的三个配置指针：

```c
rps_http_request_t *r;

r->main_conf = http_ctx.main_conf;          // 始终是 http 级
r->srv_conf  = matched_server_ctx.srv_conf; // 匹配到的 server 级
r->loc_conf  = matched_loc_ctx.loc_conf;    // 匹配到的 location 级

// 模块内部通过宏取值：
#define rps_http_get_loc_conf(r, module)  (r->loc_conf[module.ctx_index])

// proxy 模块在 handler 中拿到的就是合并后的最终值：
rps_http_proxy_loc_conf_t *plcf = rps_http_get_loc_conf(r, rps_http_proxy_module);
// plcf->connect_timeout 已经是最终生效的值
```

#### 6.6 总结

| 问题 | 答案 |
|---|---|
| 每个模块的 main_conf 都有 server 数组？ | **否**，只有 core 模块有 `servers` 数组 |
| server 怎么管理的？ | core 的 main_conf 里有一个 `rps_array_t servers`，存每个 server 的容器指针 |
| 父子级配置如何关联？ | 子级的 `main_conf`/`srv_conf` 是**指针**指向父级，不拷贝数组 |
| 配置什么时候合并？ | 全部解析完后，在 `postconfiguration` 中逐模块调 `merge_srv_conf` / `merge_loc_conf` |
| UNSET 怎么覆盖？ | 每个模块自己的 merge 函数用 `rps_conf_merge_*` 宏完成 |

### 7. 模块间依赖关系

```
rps_http_request  (基础设施)
        ▲
        │
    ┌───┴───┐
    │       │
http_core  http_proxy ──── http_upstream
    │
    └── 创建 listening，启动阶段引擎
```

### 8. 分步实施建议

**第 1 步：`rps_http_request` + 请求解析**（不需要额外模块）

- 定义 `rps_http_request_t`、`rps_chain_t`、headers 结构体
- 实现请求行和请求头的**状态机解析器**
- 实现一个简单的 `rps_http_send_response(r, status, body)`，把 `"HTTP/1.1 200 OK\r\n..."` 拼出来发回去

此时就能在 `worker_thread` 里做到：accept → 读请求 → 解析请求行 → 返回 200 + "Hello World"

**第 2 步：`http_core` 模块**

- 实现 `http {}` / `server {}` / `location {}` 的块解析
- 实现 `listen`、`server_name` 指令
- 实现 FIND_CONFIG：根据 Host + URI 匹配 server 和 location
- 阶段引擎基础（先只跑 CONTENT 阶段，其他阶段空数组）

**第 3 步：`http_proxy` 模块**（先不做 upstream，直接用 IP:port）

- 实现 `proxy_pass` 指令
- 实现 `rps_proxy_handler`：连后端 → 转发请求 → 读响应 → 写回客户端
- 基本代理头处理：`Host`、`X-Forwarded-For`

**第 4 步：`http_upstream` 模块**（完善生产级特性）

- 连接池复用
- 轮询负载均衡
- 失败重试 + 健康检查

这样可以最快的速度把"代理转发"跑通，然后逐步完善。

### 9. 线程模式 vs 事件模式的差异点

两种模式处理 HTTP 的逻辑**完全共享**，差异只在 I/O 方式和调用路径：

| | 线程模式 | 事件模式（epoll） |
|---|---|---|
| 连接入口 | `accept` → 新线程执行 `proxy_handler` | `accept` → conn 注册到 epoll，fd 就绪时回调 proxy_handler |
| 读请求 | `read()` 阻塞读，状态机解析 | 非阻塞 `read()`，未读完返回 `RPS_AGAIN`，下次 EPOLLIN 再重入 |
| 连后端 | 阻塞 `connect()` | 非阻塞 `connect()`，加入 epoll 等 EPOLLOUT |
| 写响应 | 阻塞 `write()` | 非阻塞 `write()`，写不完注册 EPOLLOUT，下次继续写 |
| handler 返回值 | 直接返回 RPS_OK/RPS_ERROR | RPS_AGAIN 表示暂停，下次事件就绪后从上次断点继续 |

**实现重点：** `rps_proxy_handler` 的核心转发逻辑（构造请求、解析响应）写成一个函数，线程模式和事件模式都调用它。事件模式下，I/O 操作前保存当前状态，RPS_AGAIN 时恢复状态并重新注册到 epoll。

---

## 构建

```bash
mkdir -p build && cd build
cmake .. && make
```
