# RPS 模块接口文档

## 目录

1. [架构概述](#架构概述)
2. [rps_core_module — 核心模块](#rps_core_module)
3. [rps_event_module — Event 块解析模块](#rps_event_module)
4. [rps_event_core_module — Event 配置模块](#rps_event_core_module)
5. [rps_epoll_module — Epoll 事件引擎](#rps_epoll_module)
6. [rps_http_module — HTTP 块解析模块](#rps_http_module)
7. [rps_http_core_module — HTTP 核心模块](#rps_http_core_module)
8. [rps_http_proxy_module — HTTP 反向代理模块](#rps_http_proxy_module)
9. [附录：通用结构定义](#附录)

---

## 架构概述

### 模块类型

| 类型常量 | 值 | 说明 |
|---|---|---|
| `RPS_CORE_MODULE` | 0 | 核心基础设施模块，配置挂载在 `conf_ctx[module->index]` |
| `RPS_EVENT_MODULE` | 1 | 事件驱动模块，配置挂载在 `event_conf[module->ctx_index]` |
| `RPS_HTTP_MODULE` | 2 | HTTP 处理模块，支持三级配置（main / srv / loc） |

### 通用模块结构 (`rps_module_s`)

```c
struct rps_module_s {
    rps_uint_t   index;        // 模块在 rps_modules[] 中的全局索引
    rps_uint_t   ctx_index;    // 同类型模块内的索引（仅 EVENT / HTTP 类型有效）
    rps_str_t    name;         // 模块名称
    char        *version;      // 版本号
    void        *ctx;          // 模块上下文（钩子函数表，类型由 module type 决定）
    rps_command_t *commands;   // 支持的配置指令数组
    rps_uint_t   type;         // 模块类型

    // 生命周期钩子
    rps_int_t  (*init_module)(rps_cycle_t *cycle);
    rps_int_t  (*init_process)(rps_cycle_t *cycle);
    void       (*exit_process)(rps_cycle_t *cycle);
    void       (*exit_master)(rps_cycle_t *cycle);
};
```

### 配置体系

- **CORE_MODULE**：单级配置，`conf_ctx[module->index]` 直接指向配置结构体。
- **EVENT_MODULE**：单级配置，存储在 `rps_event_container_t` 的 `event_conf[]` 数组中。
- **HTTP_MODULE**：三级配置，存储在 `rps_http_conf_container_t` 中：
  - `main_conf[]` — `http {}` 级别
  - `srv_conf[]` — `server {}` 级别
  - `loc_conf[]` — `location {}` 级别

配置继承关系：`main → server → location`，通过 `merge_srv_conf` / `merge_loc_conf` 钩子实现。

---

## rps_core_module

### 概述

核心基础设施模块，管理系统级配置（进程数、守护进程模式、PID 文件路径）。所有配置指令定义在主配置文件的顶层（`RPS_MAIN_CONF`）。

### 基础信息

| 属性 | 值 |
|---|---|
| 类型 | `RPS_CORE_MODULE` |
| 名称 | `core` |
| 版本 | `1.0.0` |
| 源文件 | `src/core/rps_core_module.c`, `src/core/rps_core_module.h` |

### 上下文类型

```c
typedef struct {
    rps_str_t    name;
    void      *(*create_conf)(rps_cycle_t *cycle);
    char      *(*init_conf)(rps_cycle_t *cycle);
} rps_core_module_ctx_t;
```

### 配置结构 (`rps_core_conf_t`)

```c
typedef struct {
    rps_uint_t   daemon;           // 是否开启守护进程（1/0）
    rps_uint_t   worker_processes; // 工作进程数
    rps_str_t    pid;              // PID 文件路径
    rps_str_t    user;             // 运行用户（预留，未实现）
} rps_core_conf_t;
```

### 配置指令

| 指令 | 参数 | 上下文 | 说明 |
|---|---|---|---|
| `worker_processes` | `number \| auto` | main | 工作进程数。`auto` 等价于 1。不允许重复设置。 |
| `daemon` | `on \| off` | main | 是否以守护进程模式运行。不允许重复设置。 |
| `pid` | `path` | main | PID 文件路径。默认 `run_pid.conf`。不允许重复设置。 |

### 钩子函数

| 钩子 | 函数 | 说明 |
|---|---|---|
| `create_conf` | `rps_core_create_conf` | 分配 `rps_core_conf_t`，将 `worker_processes`、`daemon` 设为 `RPS_CONF_UNSET_UINT`，`pid` 默认设为 `run_pid.conf`。 |
| `init_conf` | `rps_core_init_conf` | 填充默认值：`daemon` 默认 0，`worker_processes` 默认 1。 |

### 生命周期钩子

无。`init_module`、`init_process`、`exit_process`、`exit_master` 均为 NULL。

---

## rps_event_module

### 概述

Event 块指令解析模块。负责识别配置文件中的 `event { }` 块，创建 event 配置容器，并触发子块解析。

### 基础信息

| 属性 | 值 |
|---|---|
| 类型 | `RPS_CORE_MODULE` |
| 名称 | `event` |
| 版本 | `1.0.0` |
| 源文件 | `src/event/rps_event_module.c` |

### 上下文类型

`rps_core_module_ctx_t`（同 core_module）。

### 配置结构

```c
typedef struct {
    void **event_conf; // 所有 EVENT_MODULE 的配置指针数组
} rps_event_container_t;
```

### 配置指令

| 指令 | 参数 | 上下文 | 说明 |
|---|---|---|---|
| `event` | 无（块指令） | main | 进入 `event {}` 块解析。创建容器后递归解析块内指令。 |

### 钩子函数

| 钩子 | 函数 | 说明 |
|---|---|---|
| `create_conf` | `rps_event_create_conf` | 分配 `rps_event_container_t`，遍历所有 `RPS_EVENT_MODULE` 类型模块，依次调用其 `create_conf` 创建配置结构体，存入 `event_conf[]`。 |
| `init_conf` | `rps_event_init_conf` | 遍历所有 `RPS_EVENT_MODULE` 类型模块，依次调用其 `init_conf` 填充默认值。 |

### 生命周期钩子

无。

---

## rps_event_core_module

### 概述

Event 核心配置模块，管理事件引擎选择和连接数限制。提供 `use` 和 `worker_connections` 两条指令，并在 worker 进程初始化时创建全局连接池。

### 基础信息

| 属性 | 值 |
|---|---|
| 类型 | `RPS_EVENT_MODULE` |
| 名称 | `event_core` |
| 版本 | `1.0.0` |
| 源文件 | `src/event/modules/rps_event_core.c` |

### 上下文类型

```c
typedef struct rps_event_module_s {
    void      *(*create_conf)(rps_cycle_t *cycle);
    char      *(*init_conf)(rps_cycle_t *cycle);
    rps_int_t  (*add_event)(rps_event_t *ev, rps_uint_t event);
    rps_int_t  (*del_event)(rps_event_t *ev, rps_uint_t event);
    rps_int_t  (*process_events)(rps_cycle_t *cycle, rps_msec_t timer);
    rps_int_t  (*init_process)(rps_cycle_t *cycle);
} rps_event_module_t;
```

### 配置结构 (`rps_event_conf_t`)

```c
typedef struct {
    rps_str_t    use;                // 事件通知模型："epoll" / "io_uring" / "threads"
    rps_uint_t   worker_connections; // 每个 worker 进程最大连接数
} rps_event_conf_t;
```

### 配置指令

| 指令 | 参数 | 上下文 | 说明 |
|---|---|---|---|
| `use` | `epoll \| io_uring \| threads` | event | 选择事件通知模型。选择 `threads` 会将 `cycle->if_pthread` 设为 1。 |
| `worker_connections` | `number` | event | 每个 worker 进程的最大连接数。 |

### 钩子函数（配置相关）

| 钩子 | 函数 | 说明 |
|---|---|---|
| `create_conf` | `rps_event_core_create_conf` | 分配 `rps_event_conf_t`，`use` 默认 `"epoll"`，`worker_connections` 设为 `RPS_CONF_UNSET_UINT`。 |
| `init_conf` | `rps_event_core_init_conf` | 填充默认值：`worker_connections` 默认 10。 |

### 钩子函数（事件引擎操作）

本模块不实现事件引擎操作（`add_event` / `del_event` / `process_events` 均为 NULL），这些由具体的事件引擎模块（如 epoll）提供。

### 生命周期钩子

| 钩子 | 函数 | 说明 |
|---|---|---|
| `init_process` | `rps_event_core_init_process` | **在 worker 进程启动时调用**。根据 `worker_connections` 配置分配全局连接池 `cycle->connections`、读事件池 `cycle->reads`、写事件池 `cycle->writes`，并将所有连接加入空闲链表 `cycle->free_connection`。 |

---

## rps_epoll_module

### 概述

Epoll 事件引擎实现，提供基于 Linux `epoll` 的 I/O 多路复用能力。

### 基础信息

| 属性 | 值 |
|---|---|
| 类型 | `RPS_EVENT_MODULE` |
| 名称 | `epoll` |
| 版本 | `1.0.0` |
| 源文件 | `src/event/modules/rps_epoll_module.c` |

### 上下文类型

`rps_event_module_t`

### 配置结构

复用 `rps_event_conf_t`（与 event_core_module 共享）。

### 配置指令

无。该模块不提供用户可配置的指令。

### 钩子函数（配置相关）

| 钩子 | 函数 | 说明 |
|---|---|---|
| `create_conf` | `rps_epoll_create_conf` | 分配 `rps_event_conf_t`，`use` 默认 `"epoll"`，`worker_connections` 设为 `RPS_CONF_UNSET_UINT`。 |
| `init_conf` | `rps_epoll_init_conf` | 空实现，无额外校验。 |

### 钩子函数（事件引擎操作）

| 钩子 | 函数 | 说明 |
|---|---|---|
| `init_process` | `rps_epoll_init_process` | 创建 `epoll` 文件描述符（`epoll_create1(EPOLL_CLOEXEC)`），分配 `event_list` 缓冲区（默认 512 个事件），并将 `cycle->event_engine` 指向自身的 `rps_epoll_module_ctx`。如果 `cycle->if_pthread == 1` 则跳过（使用线程模式）。 |
| `add_event` | `rps_epoll_add_event` | 向 epoll 注册事件。若事件已激活则 `EPOLL_CTL_MOD`，否则 `EPOLL_CTL_ADD`。读写事件使用 `EPOLLEXCLUSIVE` 标志避免惊群。 |
| `del_event` | `rps_epoll_del_event` | 从 epoll 移除事件（`EPOLL_CTL_DEL`），重置 `active` 和 `epoll_events` 标志。 |
| `process_events` | `rps_epoll_process_events` | 调用 `epoll_wait` 等待 I/O 事件。遍历就绪事件列表，设置 `eof`/`error`/`ready`/`write` 标志，调用对应事件的 `handler` 回调。`EINTR` 中断时返回 OK。 |

### 生命周期钩子

| 钩子 | 函数 | 说明 |
|---|---|---|
| `init_process` | `rps_epoll_init_process` | 在 worker 进程启动时创建 epoll fd 和事件缓冲区，注册为当前进程的事件引擎。 |

---

## rps_http_module

### 概述

HTTP 块指令解析模块。负责识别配置文件中的 `http { }` 块，创建 HTTP 三级配置容器，为所有 HTTP 模块创建 main/srv/loc 三级配置，触发子块解析，并在解析完成后调用各模块的 `postconfiguration` 钩子并初始化阶段引擎。

### 基础信息

| 属性 | 值 |
|---|---|
| 类型 | `RPS_CORE_MODULE` |
| 名称 | `http_module` |
| 版本 | `1.0.0` |
| 源文件 | `src/http/rps_http_module.c` |

### 配置结构

```c
typedef struct {
    void **main_conf; // http {} 级配置指针数组
    void **srv_conf;  // server {} 级配置指针数组
    void **loc_conf;  // location {} 级配置指针数组
} rps_http_conf_container_t;
```

### 配置指令

| 指令 | 参数 | 上下文 | 说明 |
|---|---|---|---|
| `http` | 无（块指令） | main | 进入 `http {}` 块解析。创建根容器，遍历所有 HTTP 模块调用 `create_main_conf` / `create_srv_conf` / `create_loc_conf`，递归解析块内指令，解析完成后调用各模块 `postconfiguration` 并初始化阶段引擎。 |

### 钩子函数

| 钩子 | 函数 | 说明 |
|---|---|---|
| `create_conf` | `rps_http_module_create_conf` | 分配 `rps_http_conf_container_t`，为三级 `main_conf` / `srv_conf` / `loc_conf` 各分配 `rps_http_modules_n` 个指针的数组（只分配容器，不创建模块配置）。 |

### 生命周期钩子

无。

---

## rps_http_core_module

### 概述

HTTP 核心模块，提供 `server {}`、`location {}` 配置块，虚拟主机名和端口绑定，请求路由匹配（FIND_CONFIG phase），以及默认的 content handler 兜底响应。

### 基础信息

| 属性 | 值 |
|---|---|
| 类型 | `RPS_HTTP_MODULE` |
| 名称 | `http_core` |
| 版本 | `1.0.0` |
| 源文件 | `src/http/modules/rps_http_core_module.c`, `src/http/modules/rps_http_core_module.h` |

### 上下文类型

```c
typedef struct {
    rps_int_t (*preconfiguration)(rps_conf_t *cf);
    rps_int_t (*postconfiguration)(rps_conf_t *cf);
    void *(*create_main_conf)(rps_conf_t *cf);
    void *(*create_srv_conf)(rps_conf_t *cf);
    void *(*create_loc_conf)(rps_conf_t *cf);
    char *(*merge_srv_conf)(rps_pool_t *pool, void *parent, void *child);
    char *(*merge_loc_conf)(rps_pool_t *pool, void *parent, void *child);
} rps_http_module_t;
```

### 三级配置结构

#### main 级别 (`rps_http_core_main_conf_t`)

```c
typedef struct {
    rps_array_t            servers;              // 所有 server 容器指针数组
    rps_uint_t             client_max_body_size; // 客户端请求体最大大小
    rps_http_phase_t       phases[11];           // 11 个阶段的 handler 注册数组
    rps_http_phase_engine_t phase_engine;        // 展平后的阶段引擎
} rps_http_core_main_conf_t;
```

#### srv 级别 (`rps_http_core_srv_conf_t`)

```c
typedef struct {
    rps_str_t    server_name;  // 虚拟主机名
    rps_uint_t   port;         // 监听端口
    rps_array_t  locations;    // 该 server 下所有 location 容器指针数组
} rps_http_core_srv_conf_t;
```

#### loc 级别 (`rps_http_core_loc_conf_t`)

```c
typedef struct {
    rps_str_t    pattern;      // location 匹配路径，如 /api/
    rps_uint_t   exact_match:1;// 是否精确匹配（`= /pattern`）
} rps_http_core_loc_conf_t;
```

### 配置指令

| 指令 | 参数 | 层级 | 说明 |
|---|---|---|---|
| `server` | 无（块指令） | main | 创建 `server {}` 子容器，继承 main 配置，为所有 HTTP 模块创建 srv/loc 级配置，递归解析块内指令。 |
| `listen` | `port` | srv | 监听端口号（数字）。固定绑定 `0.0.0.0`。 |
| `server_name` | `name` | srv | 虚拟主机名，用于 Host 头匹配。不允许重复设置。 |
| `location` | `pattern`（块指令） | srv | 创建 `location {}` 子容器，继承 srv 配置，为所有 HTTP 模块创建 loc 级配置，递归解析块内指令。 |
| `http_body_max_size` | `size` | main | 客户端请求体最大大小限制（字节）。 |

### 钩子函数（配置）

| 钩子 | 函数 | 说明 |
|---|---|---|
| `create_main_conf` | `rps_http_core_create_main_conf` | 分配 `rps_http_core_main_conf_t`，初始化 `servers` 数组和 11 个 `phases[].handlers` 数组，`client_max_body_size` 设为 UNSET。 |
| `create_srv_conf` | `rps_http_core_create_srv_conf` | 分配 `rps_http_core_srv_conf_t`，初始化 `locations` 数组，`port` 设为 UNSET，`server_name` 设为空。 |
| `create_loc_conf` | `rps_http_core_create_loc_conf` | 分配 `rps_http_core_loc_conf_t`，`pattern` 设为空，`exact_match` 设为 0。 |
| `merge_srv_conf` | `rps_http_core_merge_srv_conf` | main → srv 配置合并：port / pattern / exact_match 如果 srv 级别未设置则继承 main 级别。 |
| `merge_loc_conf` | `rps_http_core_merge_loc_conf` | srv → loc 配置合并：exact_match 如果 loc 级别未设置则继承 srv 级别。 |

### 钩子函数（模块）

| 钩子 | 函数 | 说明 |
|---|---|---|
| `postconfiguration` | `rps_http_core_postconfiguration` | **所有 HTTP 模块配置解析完成后调用**。执行以下工作：1) 注册 FIND_CONFIG phase handler（`rps_http_core_find_config_handler`）作为占位；2) 注册默认 CONTENT phase handler（`rps_http_core_default_handler`）作为兜底；3) 遍历所有 server 块，为每个 `listen` 指令创建 `rps_listening_t` 并添加到 `cycle->listening`；4) 遍历所有 HTTP 模块执行 main→server→location 三级配置合并。 |

### 阶段 Handler 函数

| Handler | 注册阶段 | 说明 |
|---|---|---|
| `rps_http_core_find_config_handler` | `RPS_HTTP_FIND_CONFIG_PHASE` | 占位 handler，实际路由匹配工作由 phase checker（`rps_http_core_find_config_phase`）完成：根据 Host 头匹配 server，根据 URI 匹配 location，将匹配到的 `srv_conf` / `loc_conf` 赋值给 `r->srv_conf` / `r->loc_conf`。Handler 本身为空函数体，仅确保展平数组中有该项。 |
| `rps_http_core_default_handler` | `RPS_HTTP_CONTENT_PHASE` | 默认内容处理器。当没有其他 content handler（如 proxy）匹配时，发送 `HTTP 200` 头并返回 `Hello from RPS!\n` 作为响应体。 |

### 生命周期钩子

无（全部为 NULL）。

---

## rps_http_proxy_module

### 概述

HTTP 反向代理模块，将匹配 location 的客户端请求转发到后端 upstream 服务器，再读取后端响应返回给客户端。

### 基础信息

| 属性 | 值 |
|---|---|
| 类型 | `RPS_HTTP_MODULE` |
| 名称 | `http_proxy` |
| 版本 | `1.0.0` |
| 源文件 | `src/http/modules/rps_http_proxy_module.c`, `src/http/modules/rps_http_proxy_module.h` |

### 配置结构（仅 location 级别）

```c
typedef struct {
    rps_str_t    proxy_pass;             // 完整 upstream URL
    rps_str_t    upstream_host;          // 解析出的后端主机名/IP
    rps_uint_t   upstream_port;          // 解析出的端口，默认 80
    rps_str_t    upstream_uri;           // URL 中的 path 部分

    rps_str_t    proxy_method;           // 覆盖请求方法（默认空 = 不覆盖）
    rps_str_t    proxy_http_version;     // 到后端的 HTTP 版本（默认 HTTP/1.1）

    rps_array_t  set_headers;            // proxy_set_header 设置的键值对数组
                                         // 元素类型: rps_http_proxy_header_t { key, value }

    rps_msec_t   connect_timeout;        // 连接后端超时（毫秒），默认 60000
    rps_msec_t   read_timeout;           // 读取后端响应超时（毫秒），默认 60000
    rps_msec_t   send_timeout;           // 发送请求到后端超时（毫秒），默认 60000

    rps_flag_t   buffering;              // 是否缓冲后端响应，默认 1
    rps_flag_t   pass_request_headers;   // 是否转发客户端请求头，默认 1
    rps_flag_t   pass_request_body;      // 是否转发客户端请求体，默认 1
} rps_http_proxy_loc_conf_t;
```

### 配置指令

| 指令 | 参数 | 层级 | 说明 |
|---|---|---|---|
| `proxy_pass` | `URL` | loc | 后端 upstream 地址。支持 `http://host:port/path` 格式，自动解析 host / port / uri。port 未指定时默认 80，path 未指定时默认 `/`。 |
| `proxy_set_header` | `key value` | loc | 设置转发到后端的自定义 header。可多次使用，每次添加一个键值对到 `set_headers` 数组。 |
| `proxy_connect_timeout` | `milliseconds` | loc | 连接后端超时时间（毫秒），默认 60000。 |
| `proxy_read_timeout` | `milliseconds` | loc | 读取后端响应超时时间（毫秒），默认 60000。 |
| `proxy_send_timeout` | `milliseconds` | loc | 发送请求到后端超时时间（毫秒），默认 60000。 |
| `proxy_buffering` | `on \| off` | loc | 是否缓冲后端响应，默认 on。 |
| `proxy_pass_request_headers` | `on \| off` | loc | 是否将客户端请求头转发到后端，默认 on。 |
| `proxy_pass_request_body` | `on \| off` | loc | 是否将客户端请求体转发到后端，默认 on。 |

### 钩子函数（配置）

| 钩子 | 函数 | 说明 |
|---|---|---|
| `create_loc_conf` | `rps_http_proxy_create_loc_conf` | 分配 `rps_http_proxy_loc_conf_t`，初始化所有字段为 UNSET/空值，初始化 `set_headers` 数组。 |
| `merge_loc_conf` | `rps_http_proxy_merge_loc_conf` | srv → loc 配置合并：proxy_pass / upstream_* / proxy_method / proxy_http_version 若 loc 未配置则继承 srv 级别；`connect_timeout` / `read_timeout` / `send_timeout` 使用 `rps_conf_init_value` 宏进行默认值填充。 |

### 钩子函数（模块）

| 钩子 | 函数 | 说明 |
|---|---|---|
| `postconfiguration` | `rps_http_proxy_postconfiguration` | 注册 `rps_http_proxy_handler` 到 `RPS_HTTP_CONTENT_PHASE`。 |

### 内部函数

| 函数 | 说明 |
|---|---|
| `rps_http_proxy_handler` | **CONTENT phase handler**。检查 `loc_conf` 中是否有 `proxy_pass` 配置，若无则返回 `RPS_DECLINED`。流程：1) 填充默认超时和标志位；2) 调用 `connect_upstream` 建立到后端的 TCP 连接（失败返回 502）；3) 调用 `send_request` 转发请求（失败返回 502）；4) 调用 `read_response` 读取后端响应并转发给客户端（失败返回 502）。 |
| `rps_http_proxy_connect_upstream` | 使用 `getaddrinfo` 解析 upstream 地址（支持域名 DNS 解析和 IP 直连），创建 TCP socket 并连接。连接成功返回 fd，失败返回 -1。**当前为阻塞模式**，每次请求都会重新建立连接。 |
| `rps_http_proxy_send_request` | 构造并发送代理请求到后端。请求格式：请求行（method URI HTTP/1.1）+ Host 头 + 自定义 `proxy_set_header` 头 + 客户端请求头（User-Agent, Content-Type, Content-Length 和通用 header 链表）+ `Connection: close` + 空行 + 请求体。 |
| `rps_http_proxy_read_response` | 循环 `recv` 从后端读取响应数据直至收到完整的 header（`\r\n\r\n`），然后将响应头和响应体分别 `send` 到客户端。**当前为阻塞模式**。 |

### 生命周期钩子

无。

---

## 附录

### HTTP 11 阶段引擎

| 阶段枚举 | 值 | 说明 | 当前注册的 handler |
|---|---|---|---|
| `RPS_HTTP_POST_READ_PHASE` | 0 | 请求头读取完毕 | 无 |
| `RPS_HTTP_SERVER_REWRITE_PHASE` | 1 | Server 级 URI 重写 | 无 |
| `RPS_HTTP_FIND_CONFIG_PHASE` | 2 | 虚拟主机 + location 匹配 | `rps_http_core_find_config_handler` |
| `RPS_HTTP_REWRITE_PHASE` | 3 | Location 级 URI 重写 | 无 |
| `RPS_HTTP_POST_REWRITE_PHASE` | 4 | 重写后检查（若 URI 变更则跳回 FIND_CONFIG） | 无 |
| `RPS_HTTP_PREACCESS_PHASE` | 5 | 访问预控 | 无 |
| `RPS_HTTP_ACCESS_PHASE` | 6 | 访问控制 | 无 |
| `RPS_HTTP_POST_ACCESS_PHASE` | 7 | 访问控制后处理 | 无 |
| `RPS_HTTP_PRECONTENT_PHASE` | 8 | 内容预处理 | 无 |
| `RPS_HTTP_CONTENT_PHASE` | 9 | 内容产生 | `rps_http_proxy_handler` → `rps_http_core_default_handler`（兜底） |
| `RPS_HTTP_LOG_PHASE` | 10 | 日志记录 | 无 |

Handler 返回值：

| 宏 | 值 | 含义 |
|---|---|---|
| `RPS_HTTP_OK` | 0 | 处理成功，继续当前 phase 的下一个 handler |
| `RPS_HTTP_ERROR` | -1 | 致命错误，中断请求 |
| `RPS_HTTP_AGAIN` | -4 | I/O 未就绪，挂起等待事件 |
| `RPS_HTTP_DECLINED` | -3 | 不处理，跳到下一个 handler |
| `RPS_HTTP_DONE` | -5 | 请求最终处理完毕（如 proxy 已发送完整响应） |

### 事件结构 (`rps_event_t`)

```c
struct rps_event_s {
    void                 *data;          // 指向关联的 rps_connection_t
    rps_event_handler_pt  handler;       // 事件就绪时的回调函数
    unsigned              write:1;       // 1: 写事件, 0: 读事件
    unsigned              active:1;      // 是否已注册到 epoll
    unsigned              ready:1;       // 事件是否已就绪
    unsigned              eof:1;         // 对端是否关闭连接
    unsigned              error:1;       // 是否发生 socket 错误
    unsigned              timedout:1;    // 是否超时
    unsigned              timer_set:1;   // 是否加入红黑树计时器
    rps_rbtree_node_t     timer;         // 红黑树超时节点
    unsigned              epoll_events;  // 当前 epoll 注册的标志（EPOLLIN/EPOLLOUT）
    rps_log_t            *log;
};
```

### 事件引擎操作接口

```c
typedef struct rps_event_module_s {
    void      *(*create_conf)(rps_cycle_t *cycle);
    char      *(*init_conf)(rps_cycle_t *cycle);
    rps_int_t  (*add_event)(rps_event_t *ev, rps_uint_t event);
    rps_int_t  (*del_event)(rps_event_t *ev, rps_uint_t event);
    rps_int_t  (*process_events)(rps_cycle_t *cycle, rps_msec_t timer);
    rps_int_t  (*init_process)(rps_cycle_t *cycle);
} rps_event_module_t;
```

`event` 参数为 `RPS_READ_EVENT` (0x01) 或 `RPS_WRITE_EVENT` (0x02) 的按位或组合。

### 模块注册顺序

`rps_modules[]` 数组中的注册顺序：

| 序号 | 模块 | 类型 |
|---|---|---|
| 0 | `rps_core_module` | CORE |
| 1 | `rps_event_core_module` | EVENT |
| 2 | `rps_event_module` | CORE |
| 3 | `rps_epoll_module` | EVENT |
| 4 | `rps_http_module` | CORE |
| 5 | `rps_http_core_module` | HTTP |
| 6 | `rps_http_proxy_module` | HTTP |
