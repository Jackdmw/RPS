# Epoll 事件模块 + 事件循环 + I/O 处理器 实现日志

## 2026-05-23

### 概览

实现了从 socket 数据到达 → 解析 → 阶段引擎 → 响应的完整链路基础。

---

### 一、Epoll 事件模块 (`src/event/modules/rps_epoll_module.c`)

新建文件，约 200 行。实现 `rps_event_module_t` 全部 6 个回调：

| 回调 | 功能 |
|------|------|
| `create_conf` | 分配 `rps_event_conf_t`，默认 use="epoll" |
| `init_conf` | 无额外校验 |
| `init_process` | `epoll_create1(EPOLL_CLOEXEC)` + 分配事件列表 + 注册为 `cycle->event_engine` |
| `add_event` | `epoll_ctl(ADD/MOD)`，支持读写事件 |
| `del_event` | `epoll_ctl(DEL)` |
| `process_events` | `epoll_wait` → 遍历就绪事件 → EPOLLIN/EPOLLOUT/EPOLLERR → 调用 `ev->handler(ev)` |

关键设计决策：
- epoll fd 和 event_list 使用模块内 static 变量（每进程单例）
- 使用 level-triggered 模式（非 EPOLLET），更易调试
- `epoll_wait` 超时由 `rps_msec_t timer` 控制，-1 表示无限等待

---

### 二、事件模块接口扩展 (`src/event/rps_event.h`)

`rps_event_module_t` 新增 3 个函数指针：

```c
rps_int_t (*add_event)(rps_event_t *ev, rps_uint_t event);
rps_int_t (*del_event)(rps_event_t *ev, rps_uint_t event);
rps_int_t (*process_events)(rps_cycle_t *cycle, rps_msec_t timer);
rps_int_t (*init_process)(rps_cycle_t *cycle);
```

新增事件标志：`RPS_READ_EVENT` (0)、`RPS_WRITE_EVENT` (1)。

为支持前置声明解决循环包含，给匿名 struct 加了 tag `rps_event_module_s`。
`rps_cycle.h` 中新增 `event_engine` 和 `event_data` 字段。

---

### 三、连接 / I/O 基础 (`src/core/rps_connection.c`)

| 函数 | 实现 |
|------|------|
| `rps_set_nonblocking` | `fcntl(F_GETFL)` → `fcntl(F_SETFL, flags \| O_NONBLOCK)` |
| `rps_unix_recv` | `recv(c->fd, buf, size, 0)` |
| `rps_unix_send` | `send(c->fd, buf, size, 0)` |

`rps_get_connection` 更新：从空闲链表取出连接后，初始化 `read->data` / `write->data` 指向连接自身。

---

### 四、Worker 事件循环 (`src/core/rps.c`)

#### 4.1 主循环

```c
for (;;) {
    timer = rps_event_find_timer();     // 当前返回 -1（无限等待）
    engine->process_events(cycle, timer);
    rps_event_expire_timers();          // 当前空实现
}
```

初始化流程：
1. 遍历 listening sockets，设非阻塞
2. 为每个 listening socket 创建连接对象，注册 `rps_event_accept` 读事件

#### 4.2 Accept handler (`rps_event_accept`)

- 循环 accept 直到 EAGAIN（避免 level-triggered 重复触发）
- `rps_get_connection` → 设 `fd` + 非阻塞 → `rps_http_create_request` → 注册 `rps_http_wait_request_handler` 读事件

#### 4.3 HTTP 读事件 handler (`rps_http_wait_request_handler`)

串联解析和阶段引擎的核心函数：
1. `rps_unix_recv` 读 socket → 追加到 `r->request_body`
2. 按 `parse_status` 依次调用：
   - `status == 0` → `rps_http_parse_request_line` → OK → `status = 1`
   - `status == 1` → `rps_http_parse_headers` → OK → `status = 2`
   - `status == 2` → 设置 `r->main_conf` → `rps_http_run_phases`
3. EAGAIN 时直接返回，等下次读事件
4. 解析错误 → `rps_http_finalize_request`

#### 4.4 定时器桩

- `rps_event_find_timer()`：返回 -1（无限等待）
- `rps_event_expire_timers()`：空实现
- TODO：遍历红黑树找最近到期定时器

#### 4.5 Bug 修复

- `SIGCHLD` 信号处理：`rps_reap = 0` → `rps_reap = 1`（旧 bug）

---

### 五、HTTP 解析常量 (`src/http/rps_http_parse.h`)

填补原有空头文件，定义：

```c
#define RPS_HTTP_PARSE_OK     1
#define RPS_HTTP_PARSE_ERROR  0
#define RPS_HTTP_PARSE_EAGIN  2
```

（`rps_http_parse.c` 中保留同值 `#define`，互不冲突）

---

### 六、模块注册

| 文件 | 变更 |
|------|------|
| `src/event/modules/rps_epoll_module.c` | **新建**，约 200 行 |
| `src/event/rps_event.h` | 扩展 `rps_event_module_t`（+4 字段），加 tag |
| `src/core/rps_cycle.h` | +`event_engine`、`event_data` |
| `src/core/rps_connection.h` | +`rps_unix_recv`、`rps_unix_send` 声明 |
| `src/core/rps_connection.c` | +`rps_set_nonblocking`、`rps_unix_recv`、`rps_unix_send`；更新 `rps_get_connection` |
| `src/core/rps.c` | +accept handler、+HTTP read handler、+事件循环、+定时器桩 |
| `src/core/rps_module.h` | +`extern rps_module_t rps_epoll_module` |
| `src/core/rps_module.c` | `rps_modules[]` 加入 `&rps_epoll_module` |
| `src/http/rps_http_parse.h` | +3 个解析返回值常量 |

---

---

### 七、Listening 注册（2026-05-23 补充）

在 `rps_http_core_postconfiguration` 中遍历所有 server 配置，
为每个 `listen <port>` 创建 `rps_listening_t` 并推入 `cycle->listening`。

实现细节：
- 当前只支持 `listen <port>`（地址固定为 `INADDR_ANY` / 0.0.0.0）
- 默认 backlog = 511
- `sockaddr` 用 `memcpy(&ls->sockaddr, &sin, sizeof(sin))` 填入（`rps_listening_s.sockaddr` 是值类型，不是指针）
- `addr_text` 格式化为 `"0.0.0.0:8080"` 并分配副本
- 绑定了端口的 server 在 `rps_open_listening_sockets` 中统一 `socket() → bind() → listen()`
- 清理了 `rps_connection.c` 中未使用的局部变量

---

### 已知限制 / TODO

1. **定时器系统**：`find_timer` / `expire_timers` 是桩，尚未实现红黑树遍历
2. **写事件 handler**：`rps_http_write_handler` 未实现——`send_header`/`send_body` 中 `rps_http_output_filter` 遇到 EAGAIN 后无法恢复发送
3. **keepalive**：`finalize_request` 后未重置请求状态重入解析循环
4. **请求体读取**：`reading_body` / `request_body_rest` 未处理
5. **HTTP/1.0**：默认 keepalive=1，HTTP/1.0 请求未根据 Connection 头修正
6. **proxy 模块**：仍为阻塞 I/O，需改为事件驱动

### 数据流总览

```
epoll_wait
  └─ EPOLLIN (listen fd)
       └─ rps_event_accept
            ├─ accept() 循环
            ├─ rps_http_create_request
            └─ add_event(client_fd, READ)
                 └─ 下次 epoll_wait
                      └─ EPOLLIN (client fd)
                           └─ rps_http_wait_request_handler
                                ├─ rps_unix_recv
                                ├─ parse_request_line
                                ├─ parse_headers
                                └─ rps_http_run_phases
                                     ├─ FIND_CONFIG  → vhost + location 匹配
                                     ├─ REWRITE
                                     ├─ ACCESS
                                     ├─ CONTENT     → proxy / default handler
                                     ├─ LOG
                                     └─ rps_http_finalize_request
```
