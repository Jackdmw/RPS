# RPS 反向代理服务器 — 阶段性实现路线图

## 当前状态总览

### 已完成 ✅
| 模块 | 说明 |
|------|------|
| 内存池 (rps_palloc) | 小块/大块分配、对齐、池销毁 |
| 字符串工具 (rps_str_t) | 比较、复制，宏封装 |
| 动态数组 (rps_array) | 自动扩容，支持结构体内嵌 |
| 块链表 (rps_list) | 大数组分块，适合存储大量元素 |
| 红黑树 (rps_rbtree) | 节点插入/删除/旋转，用于定时器 |
| 日志系统 (rps_log) | 分级、errno 翻译、写 stderr |
| 配置文件解析 (rps_conf_file) | 词法分析、指令匹配、参数校验、层级校验 |
| 模块系统 (rps_module) | 全局注册、类型分类、ctx_index 编号 |
| core 模块 | daemon、worker_processes、pid 指令 |
| event 配置块 | use (epoll/io_uring)、worker_connections |
| HTTP 配置块 | http{} → server{} → location{} 嵌套解析 |
| 缓冲区 (rps_buf) | 创建、读fd、不覆盖读 |
| 连接结构体 | accept/close/get_connection |
| Master/Worker 进程模型 | fork、信号处理、daemon |
| 基础 CLI | -c config_file |

### 有声明但未实现 / 实现不完整 ❌
| 函数 | 当前状态 |
|------|---------|
| `rps_http_create_request` | 缺少字段初始化 |
| `rps_http_close_request` | 空壳 |
| `rps_http_parse_request_line` | switch 未写完，响应行逻辑有 bug |
| `rps_http_parse_headers` | 空壳 |
| `rps_http_phase_engine` | 未开始 |
| `rps_http_send_header` | 未开始 |
| `rps_http_send_body` | 未开始 |
| `rps_http_output_filter` | 未开始 |
| `rps_http_register_phase_handler` | 未开始 |
| `rps_unix_recv` / `rps_unix_send` | 声明了未实现 |
| epoll / io_uring 事件模块 | 只有配置解析，无实际实现 |
| Worker 事件循环 | `while(1){}` 空循环 |
| `proxy_pass` 指令 | 配置文件里写了但未定义 |
| upstream 连接逻辑 | 完全不存在 |

---

## 阶段一：完善 HTTP 请求解析（数据进来）

> **目标：** 从 socket 读到字节后，能完整解析出 method、URI、args、HTTP version、所有请求头。

### 1.1 修复 `rps_http_create_request` — `src/http/rps_http_parse.c`

当前只初始化了 connection、pool、method、uri、http_version。需要补：

```c
// 请求体缓冲区 — parser 和事件循环需要往里读数据
r->request_body = rps_buf_create(r->pool, 4096);

// headers_in / headers_out 清零
rps_memzero(&r->headers_in, sizeof(rps_http_headers_in_t));
rps_memzero(&r->headers_out, sizeof(rps_http_headers_out_t));

// 响应默认 status
r->headers_out.status = 200;
r->headers_out.server = (rps_str_t)rps_string("RPS");

// 解析状态
r->parse_status = 0;

// 阶段引擎起始
r->phase = RPS_HTTP_POST_READ_PHASE;
r->phase_index = 0;

// 其余置空
r->out_chain = NULL;
r->loc_conf = NULL;
r->args = (rps_str_t)rps_null_string;
r->host = (rps_str_t)rps_null_string;
```

**为什么：** phase 不初始化会导致引擎行为不确定；request_body 不分配则第一次 read 没有缓冲区可用；headers_out.status 默认 200 是 Nginx 惯例。

### 1.2 完成 `rps_http_parse_request_line` — `src/http/rps_http_parse.c`

当前的 switch 在第 144 行断了，且分不清请求行和响应行。需要：

**请求行格式：** `GET /index.html?a=1 HTTP/1.1\r\n`

解析流程：
1. 扫描 `\r\n` 确定行尾（**已实现**）
2. 按空格拆出三个 token
3. Token 1：如果是 HTTP method → 存 `r->method`，状态切为 `REQUEST_LINE_METHOD`
4. Token 1：如果是 `HTTP/x.x` → 存 `r->http_version`，状态切为 `STATUS_LINE_VERSION`（响应行情况）
5. Token 2（请求行场景）：存 `r->uri`，状态切为 `REQUEST_LINE_URL`
6. Token 2（响应行场景）：解析状态码（`rps_atoi`）存 `headers_out.status`
7. Token 3（请求行）：存 `r->http_version`
8. Token 3（响应行）：忽略（reason phrase 如 "OK"）

**URI 中拆分 query string：**
```
如果 uri 包含 '?'：
  r->args.data = '?' 之后
  r->args.len  = uri.len - (args.data - uri.data)
  r->uri.len   = args.data - uri.data - 1  // 减1是去掉 '?' 自己
```

**为什么要拆：** `FIND_CONFIG_PHASE` 用 `r->uri`（不带 query string）去匹配 location。args 单独存放，后续传给 upstream / CGI。

**解析完成后：** `r->parse_status` 置为 1（或 `RPS_HTTP_PARSE_HEADERS`），表示进入 header 解析阶段。

### 1.3 实现 `rps_http_parse_headers` — `src/http/rps_http_parse.c`

逐行解析 `Header-Name: value\r\n`，直到遇到空行 `\r\n`。

实现思路：
1. 从 `r->request_body->pos` 开始（起始行已被 parse_request_line 消费）
2. 每行：找到 `\r\n`，中间内容按 `:` 拆成 key / value
3. key 转小写后比较（或直接用 `rps_strncasecmp`），填入对应字段
4. 需要一个宏或辅助函数来做大小写无关比较

映射关系：
| Header 名 | 存储位置 |
|-----------|---------|
| `Host` | `headers_in.host` + `r->host` |
| `User-Agent` | `headers_in.user_agent` |
| `Content-Type` | `headers_in.content_type` |
| `Content-Length` | `headers_in.content_length` + `content_length_n`（rps_atoi） |
| `Connection` | `headers_in.connection` |

遇到空行：
- `r->parse_status` 切为 2（body 解析阶段）
- 返回 `RPS_HTTP_PARSE_OK`
- 更新 `r->request_body->pos` 指向 body 起始位置

缓冲区不够时返回 `RPS_HTTP_PARSE_EAGIN`。

---

## 阶段二：实现阶段引擎（请求处理核心）

> **目标：** 请求解析完成后能进入阶段引擎，按 11 个阶段依次执行 handler。

### 2.1 添加全局 phase handler 数组 — `src/http/rps_http_parse.c`

```c
// 每个阶段一个动态数组，存放 handler 函数指针
static rps_array_t  *rps_http_phases[RPS_HTTP_LOG_PHASE + 1];
```

### 2.2 实现 `rps_http_register_phase_handler`

```c
void rps_http_register_phase_handler(rps_uint_t phase, rps_http_handler_pt handler) {
    rps_http_handler_pt *h;
    h = rps_array_push(rps_http_phases[phase]);
    *h = handler;
}
```

需要在某处先初始化这 11 个数组（在 `rps_init_cycle` 或 HTTP 模块的 `init_module` 中）。

### 2.3 实现 `rps_http_phase_engine` — `src/http/rps_http_parse.c`

```c
rps_int_t rps_http_phase_engine(rps_http_request_t *r) {
    rps_http_phase_t *ph;
    rps_http_handler_pt h;

    while (r->phase <= RPS_HTTP_LOG_PHASE) {
        ph = &rps_http_phases[r->phase];  // 需要重构为结构体数组

        if (r->phase_index >= ph->n) {
            // 当前阶段没有更多 handler，前进到下一阶段
            r->phase++;
            r->phase_index = 0;
            continue;
        }

        h = ph->handlers[r->phase_index];
        rc = h(r);

        if (rc == RPS_OK) {
            r->phase_index++;    // 同阶段下一个 handler
        } else if (rc == RPS_DECLINED) {
            r->phase++;          // 跳到下一个阶段
            r->phase_index = 0;
        } else if (rc == RPS_AGAIN) {
            return RPS_AGAIN;    // 异步，返回事件循环
        } else {
            // 错误，终止
            rps_http_close_request(r);
            return RPS_ERROR;
        }
    }

    // 所有阶段跑完，正常结束
    rps_http_close_request(r);
    return RPS_OK;
}
```

**核心逻辑：**
- `RPS_OK` → 当前 handler 处理了，继续同 phase 下一个
- `RPS_DECLINED` → 当前 handler 不处理，跳到下一 phase
- `RPS_AGAIN` → 需要等 I/O，返回事件循环，后续重新进入引擎时 resume
- `RPS_ERROR` → 任何错误直接终止请求

---

## 阶段三：实现事件模块（epoll）

> **目标：** 有一个能工作的 epoll 事件循环，能 accept 连接、读写 socket。

### 3.1 实现 epoll 事件模块 — 新建 `src/event/modules/rps_epoll_module.c`

需要实现：
- `rps_epoll_create_conf` — 创建 `rps_event_conf_t`，默认值
- `rps_epoll_init_conf` — 校验配置、初始化 epoll fd
- `rps_epoll_add_event` — `epoll_ctl(EPOLL_CTL_ADD)`
- `rps_epoll_del_event` — `epoll_ctl(EPOLL_CTL_DEL)`
- `rps_epoll_process_events` — 事件循环主体：`epoll_wait` → 遍历就绪事件 → 调用 `ev->handler(ev)`

对应的 `rps_event_module_t` 需要扩展，增加实际的事件操作方法：
```c
typedef struct {
    void        *(*create_conf)(rps_cycle_t *cycle);
    char        *(*init_conf)(rps_cycle_t *cycle);
    rps_int_t   (*add_event)(rps_event_t *ev, rps_uint_t events);
    rps_int_t   (*del_event)(rps_event_t *ev, rps_uint_t events);
    rps_int_t   (*process_events)(rps_cycle_t *cycle, rps_msec_t timer);
} rps_event_module_t;
```

### 3.2 初始化连接池和事件数组 — `rps_worker_process_init`

```c
// 从 event 配置中读取 worker_connections
cycle->connections = rps_pcalloc(pool, sizeof(rps_connection_t) * worker_conns);
cycle->reads = rps_pcalloc(pool, sizeof(rps_event_t) * worker_conns);
cycle->writes = rps_pcalloc(pool, sizeof(rps_event_t) * worker_conns);

// 构建空闲连接链表
for (i = 0; i < worker_conns - 1; i++) {
    cycle->connections[i].data = &cycle->connections[i + 1];
}
cycle->free_connection = &cycle->connections[0];
```

### 3.3 实现 accept handler — `rps_event_accept`

```c
void rps_event_accept(rps_event_t *ev) {
    // 1. accept() 获取 conn_fd
    // 2. rps_get_connection() 从空闲链表取连接
    // 3. 设置 connection->fd = conn_fd
    // 4. 设置非阻塞
    // 5. 创建 rps_http_request_t → connection->data = request
    // 6. 注册读事件，handler = rps_http_wait_request_handler
    // 7. epoll_ctl 添加读事件
}
```

### 3.4 实现 Worker 事件循环 — `rps_worker_process_cycle`

```c
while (1) {
    // 查找红黑树中最近的定时器
    timer = rps_event_find_timer();

    // 等待事件（epoll_wait）
    rps_event_process_events(cycle, timer);

    // 处理到期的定时器
    rps_event_expire_timers();
}
```

---

## 阶段四：HTTP 读事件和数据流转

> **目标：** 连接上有数据到达时，读入 buffer → 解析 → 进入阶段引擎。

### 4.1 实现 `rps_http_wait_request_handler`

```c
void rps_http_wait_request_handler(rps_event_t *ev) {
    rps_connection_t *c = ev->data;
    rps_http_request_t *r = c->data;

    // 1. 从 socket 读数据到 r->request_body
    n = rps_buf_read_fd_no_cover(r->request_body, c->fd, 4096);

    // 2. 根据 parse_status 调用对应解析
    if (r->parse_status == 0) {
        rc = rps_http_parse_request_line(r);
    }
    if (r->parse_status == 1) {
        rc = rps_http_parse_headers(r);
    }

    // 3. 解析完成后进入阶段引擎
    if (r->parse_status == 2) {
        rps_http_phase_engine(r);
    }
}
```

### 4.2 实现 `rps_unix_recv` / `rps_unix_send` — `src/core/rps_connection.c`

```c
ssize_t rps_unix_recv(rps_connection_t *c, u_char *buf, size_t size) {
    return recv(c->fd, buf, size, 0);
}
ssize_t rps_unix_send(rps_connection_t *c, u_char *buf, size_t size) {
    return send(c->fd, buf, size, 0);
}
```

### 4.3 完善 `rps_get_connection`

当前只取了空闲连接，但没设置读写事件。需要：
```c
c->read = &cycle->reads[connection_index];
c->write = &cycle->writes[connection_index];
c->read->data = c;
c->write->data = c;
```

---

## 阶段五：实现响应发送（数据出去）

> **目标：** 请求处理完后能把响应发回客户端。

### 5.1 实现 `rps_http_send_header`

1. 构造状态行：`HTTP/1.1 200 OK\r\n`
2. 遍历 `r->headers_out`，输出 `Content-Type: text/html\r\n` 等
3. 追加 `\r\n`（空行分隔）
4. 创建 `rps_chain_t` 节点，链入 `r->out_chain`
5. 调用 `rps_http_output_filter(r, r->out_chain)`

需要一个状态码→描述字符串的映射表：
```c
static const char *http_status_lines[] = {
    [200] = "200 OK",
    [400] = "400 Bad Request",
    [404] = "404 Not Found",
    [500] = "500 Internal Server Error",
    [502] = "502 Bad Gateway",
    [503] = "503 Service Unavailable",
    ...
};
```

### 5.2 实现 `rps_http_send_body`

1. 创建 `rps_chain_t` 包住 body buffer
2. 追加到 `r->out_chain` 尾部
3. 调用 `rps_http_output_filter`

### 5.3 实现 `rps_http_output_filter`

1. 遍历 `r->out_chain`
2. 对每个 buffer 调用 `rps_unix_send`
3. 遇到 `EAGAIN` → 停止，注册写事件，返回 `RPS_AGAIN`
4. `send` 返回 0 → 连接关闭
5. 发送完的 chain 节点释放回 pool

### 5.4 实现写事件 handler

```c
void rps_http_write_handler(rps_event_t *ev) {
    // 继续调用 rps_http_output_filter
    // 如果全部写完，切回读事件等待下一个请求
}
```

---

## 阶段六：FIND_CONFIG_PHASE 和 location 匹配

> **目标：** 请求进来后能根据 URI 匹配到对应的 location 配置。

### 6.1 实现 `rps_http_find_location` — 在 `rps_http_core_module.c` 中

```c
static void *rps_http_find_location(rps_http_request_t *r) {
    // 1. 从 connection->listenling->servers 拿到当前 server 配置
    // 2. 遍历 server 的 locations 数组
    // 3. 用 r->uri 做前缀匹配
    // 4. 找到最长匹配的 location
    // 5. r->loc_conf = location->loc_conf
}
```

### 6.2 注册 FIND_CONFIG_PHASE handler

在 `rps_http_core_module` 的 `postconfiguration` 中调用：
```c
rps_http_register_phase_handler(RPS_HTTP_FIND_CONFIG_PHASE, rps_http_find_location_handler);
```

---

## 阶段七：实现反向代理核心

> **目标：** 请求能转发到后端服务器，响应能透传回客户端。

### 7.1 定义 upstream 相关结构体

在 `src/http/` 下新建文件或扩展 `rps_http_core.h`：

```c
typedef struct {
    rps_str_t       host;       // 后端 IP
    rps_uint_t      port;       // 后端端口
    rps_sockaddr_t  sockaddr;   // 解析后的地址
} rps_upstream_server_t;

typedef struct {
    rps_str_t               name;       // upstream 名称
    rps_array_t             servers;    // rps_upstream_server_t 数组
    rps_uint_t              current;    // 轮询下标 (round-robin)
} rps_upstream_t;
```

### 7.2 实现 `proxy_pass` 指令的 set 函数

在 `rps_http_core_module.c` 中添加：
```c
// 在 location 级别注册
{
    rps_string("proxy_pass"),
    RPS_HTTP_LOC_CONF | RPS_CONF_TAKE1,
    rps_set_proxy_pass,
    RPS_CONF_BELONG_HTTP_LOC,
    offsetof(rps_http_core_loc_conf_t, upstream),
    NULL
},
```

`rps_set_proxy_pass` 解析 `proxy_pass http://backend:8080`，提取 host 和 port，存到 location 配置中。

### 7.3 扩展 `rps_http_core_loc_conf_t`

```c
typedef struct {
    rps_str_t      pattern;
    rps_uint_t     exact_match:1;
    rps_str_t      upstream;       // proxy_pass 的目标地址
} rps_http_core_loc_conf_t;
```

### 7.4 实现 proxy 内容 handler — `rps_http_proxy_handler`

注册到 `RPS_HTTP_CONTENT_PHASE`：

```c
rps_int_t rps_http_proxy_handler(rps_http_request_t *r) {
    // 1. 从 r->loc_conf 拿到 upstream 地址
    // 2. 创建到后端的新连接（非阻塞 connect）
    // 3. 构造转发请求：
    //    - 请求行：METHOD URI HTTP/1.1\r\n
    //    - 加上原始请求头（可能修改 Host）
    //    - 如果有 body，转发 body
    // 4. 发送到后端
    // 5. 如果 connect/send 没完成 → 注册写事件 → 返回 RPS_AGAIN
    // 6. 读取后端响应：
    //    - 解析状态行
    //    - 解析响应头
    //    - 读取 body
    // 7. 把后端响应写入 r->headers_out 和 r->out_chain
    // 8. 调用 rps_http_send_header / rps_http_send_body
}
```

**关键点：**
- 非阻塞 connect → 需要处理 `EINPROGRESS`
- 后端响应解析可以复用 `rps_http_parse_request_line`（响应行模式）
- 请求体和响应体都是流式的，buffer 不够时返回 `RPS_AGAIN`
- 需要处理后端断开、超时等错误 → 返回 502

### 7.5 扩展 `rps_http_request_t` 支持 upstream

```c
typedef struct rps_http_request_s {
    // ... 已有字段 ...
    rps_connection_t       *upstream;     // 到后端的连接（proxy 时用）
    rps_buf_t              *upstream_buf; // 后端响应缓冲区
    // ...
} rps_http_request_t;
```

---

## 阶段八：收尾和错误处理

### 8.1 实现 `rps_http_close_request`

```c
void rps_http_close_request(rps_http_request_t *r) {
    if (r->upstream) {
        rps_close_connection(r->upstream);
    }
    if (r->connection && r->connection->data == r) {
        r->connection->data = NULL;
    }
    if (r->pool) {
        rps_destroy_pool(r->pool);
    }
}
```

### 8.2 完善 `rps_close_connection`

当前只 close fd。需要：
1. 从 epoll 中删除读写事件
2. 归还连接到空闲链表
3. 销毁连接池

### 8.3 生命周期整合

```
accept
  → rps_http_create_request
  → read 事件: rps_http_wait_request_handler
    → rps_http_parse_request_line   (parse_status = 0→1)
    → rps_http_parse_headers        (parse_status = 1→2)
    → rps_http_phase_engine
      → FIND_CONFIG_PHASE  → 匹配 location
      → CONTENT_PHASE      → proxy_handler
        → 连接后端
        → 转发请求
        → 读取后端响应
      → send_header
      → send_body
      → output_filter (可能多轮 EAGAIN)
      → LOG_PHASE
      → rps_http_close_request
```

---

## 附：需要修复的已知 Bug

### Bug 1: `rps_http_parse_request_line` 第 123-132 行
响应行场景（`HTTP/1.0 200 OK`）中，第一个 token `HTTP/1.0` 被识别后，把第二个 token 错误地赋给了 `r->uri`。应该是：
- 第 122-133 行：token1 = HTTP版本 → `r->http_version = arg`，状态切为 `STATUS_LINE_VERSION`
- 新增 case `STATUS_LINE_VERSION`：token2 = 状态码 → `r->headers_out.status = rps_atoi(..)`，状态切为 `STATUS_LINE_STATUS`

### Bug 2: `rps_conf_read_token` 行号计数
第 178 行，word 模式下遇到换行时 `cf->conf_file->line++`，但此时 buffer 的 pos 已前进。如果 word 的最后一个字符在换行前，这个换行会被 skip 掉；但如果 word 刚好在行尾结束，下一次 `rps_conf_read_token` 调用时状态是 PREPARE，遇到 `\n` 时又会计一次行号——导致行号可能重复计数。

需要统一行号计数逻辑：只在 PREPARE 状态下遇到 `\n` 时 `line++`。

### Bug 3: `rps_http_parse_request_line` 未更新 parse_status
解析成功后应该设置 `r->parse_status = 1`，否则 `rps_http_parse_headers` 不会被调用。

### Bug 4: CMakeLists 合并问题
`src/core/rps_conf_file.c` 第 401 行 `if(cmd->set(cf,cmd,ctx) != NULL)` — set 函数成功时返回 `RPS_CONF_OK`（即 NULL/0），所以这里的判断是正确的（非 NULL 才是错误）。但 `rps_conf_set_str_slot` 和 `rps_conf_set_num_slot` 成功时返回的正确值应该是 `RPS_CONF_OK`（定义为 char* 的 NULL），目前它们返回的是 `RPS_CONF_OK`（即 0，即 NULL），这是对的。需要确保所有 set 函数返回值一致。

---

## 各阶段预估工作量和依赖关系

```
阶段一 (HTTP 解析)      ████████░░░░  约 2-3 天    无依赖
阶段二 (阶段引擎)        ██████░░░░░░  约 1-2 天    依赖阶段一
阶段三 (epoll)          ██████████░░  约 2-3 天    无依赖
阶段四 (数据流转)        ████████░░░░  约 1-2 天    依赖阶段一 + 三
阶段五 (响应发送)        ████████░░░░  约 1-2 天    依赖阶段二
阶段六 (location 匹配)   ████░░░░░░░░  约 1 天      依赖阶段二
阶段七 (反向代理)        ████████████  约 3-5 天    依赖阶段四 + 五 + 六
阶段八 (收尾)           ████░░░░░░░░  约 1 天      依赖所有
```

推荐按 **一 → 三 → 四 → 二 → 五 → 六 → 七 → 八** 的顺序推进，因为先打通数据链路（socket→buffer→解析），再接入阶段引擎，能边写边测。
