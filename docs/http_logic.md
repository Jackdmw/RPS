# HTTP 请求处理逻辑

> 本文档定义 HTTP 请求处理的完整流程。代码应按照此文档实现。

---

## 核心设计：连接与请求分离

**一个原则**:

```
连接永远在请求之后释放。
请求结束 (finalize) → 根据 keepalive + 成功/失败 → 决定连接的命运。
```

这意味着 `complete_request(c)` 只有一个语义：**请求已结束，连接何去何从**。它不会在请求未结束时被调用。

**事件属于连接，不属于某个请求。** 一个连接只有 c->read / c->write 两个事件。
请求通过 `r->connection` 使用这些事件来发送响应，但不持有事件的所有权。

```
解析完成
  ├── c->data = NULL           // 解绑。连接状态不在此处决定
  └── r 进入阶段引擎（r->keepalive 可能被 handler 修改，如代理置 0）
```

**请求结束时确定连接命运**:
```
finalize(r, rc):
  若 rc != OK → keepalive = 0
  否则 keepalive = r->keepalive（解析器初值 + handler 可能修改后的最终值）
  c->close = !keepalive          // 此时才决定连接命运
  销毁 r

complete_request(c):
  c->close? → free(c) : create new r + register event
```

**为什么不在解析时定 c->close**: 解析器只知道 HTTP 协议层面的 keepalive 意愿。
但 handler 可能强制关闭（如代理），请求处理过程中的错误也可能导致不可恢复。
现代服务器（nginx 等）的做法：只要能发完整响应，即使 500 也保持 keepalive；
但 handler 有权置 `keepalive=0`。最终决定权在 finalize。

**好处**:
- 连接管理不依赖请求状态。complete_request 不需要检查 `r->upstream`。
- 请求生命周期独立：finalize 只销毁自己，不操心连接的命运。
- complete_request 是独立函数，同步和异步路径都可以调用。

---

## 整体链路

```
accept → [创建请求] → [读+解析] → 解耦 → [阶段引擎] → return
                                         │
                                  请求结束(同步或异步)
                                    finalize(r) → complete_request(c)
```

---

## 1. 连接接入 (accept)

**触发**: 监听 fd 可读

1. accept() 得到 client fd，设为非阻塞
2. 从连接池取空闲连接对象 c
3. 记录客户端地址到 c
4. `r = rps_http_create_request(c)`  →  创建请求对象，r->connection = c
5. `c->data = r`
6. `add_event(c->read, READ)` + `add_timer(c->read, timeout)`

**异常**: accept 失败 → 忽略；无空闲连接 → 关闭 fd

---

## 2. 创建请求 (create_request)

**职责**: 分配请求对象及专属资源，与连接池隔离。

```
1. r->pool = rps_create_pool(4096)    // 独立请求池
2. 从 r->pool 分配 rps_http_request_t
3. parse_status = 0, keepalive = 1
4. 初始化 headers_in / headers_out 为空链表
5. headers_out 默认值: status="200 OK", server="RPS"
6. 创建 request_body buffer
7. r->connection = c
```

**边界**: 只分配和初始化。r->pool 生命周期覆盖整个请求，结束时统一销毁。

---

## 3. 读取与解析

**触发**: c->read 事件就绪

```
读事件就绪:
  ev->timedout? → finalize(r, ERROR) → complete_request(c) → return

  读 socket 到 r->request_body buffer
  对端关闭/错误 → finalize(r, ERROR) → complete_request(c) → return

  状态机:
    status 0: parse_request_line()  → EAGAIN? return : ERROR? finalize→complete→return : status=1
    status 1: parse_headers()       → EAGAIN? return : ERROR? finalize→complete→return : status=2
    status 2: check_body()          → 数据不足? return : 解析完成
```

**解析完成后的解耦**:

```
解析完成
  │
  ├── c->data = NULL    // 解绑
  └── r 进入阶段引擎
      阶段引擎返回 → 直接 return（不判断、不处理连接）
```

**规则: 谁调 finalize，谁负责 complete_request。**
- checker 判定请求结束 → checker 调 finalize(r, rc) + complete_request(c)
- checker 收到 RPS_AGAIN → checker 返回 RPS_OK，不调 finalize
- 异步回调（代理读等）→ 回调内部调 finalize + complete_request
- 解析错误 → wait_request_handler 自身调 finalize + complete_request


**关键**: c->data 统一置 NULL。创建新 r 的工作完全交给 complete_request。

---

## 4. 阶段引擎

**职责**: 纯调度——按顺序遍历 phase handler，调用 checker，根据 checker 返回值决定
继续还是返回。引擎自身不做任何清理（不调 finalize，不调 complete_request）。

**阶段**: FIND_CONFIG → REWRITE → ACCESS → CONTENT → LOG

**引擎循环逻辑**:
```
for each phase:
  checker 返回 RPS_AGAIN → 继续（推进 phase_index）
  checker 返回 RPS_OK   → return（请求挂起或已结束）
```

**checker 的职责**:
- 调用 handler(r)
- 解释返回值，决定是否结束请求
- 如果结束请求: 调 finalize(r, rc) + complete_request(c)，返回 RPS_OK 给引擎
- 如果挂起 (RPS_AGAIN): 返回 RPS_OK 给引擎（引擎 return）
- 如果继续 (DECLINED): 返回 RPS_AGAIN 给引擎（引擎继续）

**handler 返回值 → checker 行为**:

| handler 返回 | checker 行为 | 返回给引擎 |
|-------------|-------------|-----------|
| RPS_DECLINED | 推进 phase_index | RPS_AGAIN |
| RPS_AGAIN | 直接返回 | RPS_OK |
| RPS_OK | finalize(r,OK) + complete_request(c) | RPS_OK |
| RPS_HTTP_DONE | finalize(r,OK) + complete_request(c) | RPS_OK |
| 其他 | finalize(r,rc) + complete_request(c) | RPS_OK |

**CONTENT 兜底**: 全部 handler DECLINED → checker 发送默认响应 →
finalize(r, OK) + complete_request(c) → 返回 RPS_OK。

**引擎与清理的边界**: 引擎不调 finalize，不调 complete_request。所有清理逻辑在
checker 中完成。如果 handler 已在内部调了 finalize（如代理 cleanup），必须返回
RPS_AGAIN 告诉 checker 不要再调 finalize。

---

## 5. 发送响应

```
rps_http_send_header(r):  从 r->headers_out 序列化 → send 到 r->connection->fd
rps_http_send_body(r, b): 包装 body → send
output_filter(r, chain):  遍历 chain，send()；EAGAIN → RPS_AGAIN
```

**headers_out 结构**: status, server, content_type, content_length_n, headers 链表

---

## 6. 结束请求与清理

### 6.1 finalize

```
rps_http_finalize_request(r, rc):
  1. keepalive = (rc == OK) ? r->keepalive : 0
  2. c->close = !keepalive       // ← 最终决定
  3. 如果 r->upstream: free(r->upstream)
  4. rps_destroy_pool(r->pool)   // 销毁 r，调用者不可再访问
```

finalize 是唯一设置 `c->close` 的地方。解析时不设，handler 不设。

### 6.2 连接收尾 (rps_http_complete_request)

`complete_request` 是独立函数，任何调了 finalize 的地方都必须接着调它：

```
checker:     handler→OK → finalize(r,OK) → complete_request(c)
解析错误:    finalize(r,ERR) → complete_request(c)
代理读回调:  finalize(r,OK) → complete_request(c)
代理 cleanup: finalize(r,ERR) → complete_request(c)
```

```
rps_http_complete_request(c):
  if c->close:
    rps_free_connection(c)
  else:
    // keepalive: 创建新 r，注册读事件（此时连接完全空闲）
    r_new = create_request(c)
    c->data = r_new
    add_event(c->read, READ) + add_timer(c->read, timeout)
```

**合约**: 
- 只在 finalize 后调用。complete_request 不关心旧 r 的细节（r 已销毁）。
- 新 r 的创建、挂载、事件注册全在 complete_request 中完成，语义统一。

---

## 7. upstream 层（上游连接管理）

nginx 将上游连接管理的通用逻辑（connect → send → read → cleanup）抽象为
`ngx_http_upstream` 框架。具体协议模块（proxy、fastcgi、uwsgi）只需实现
**create_request** 和 **process_header** 两个回调。

### 7.1 架构分层

```
upstream 层（通用，rps_upstream.c）:
  - 隐式状态机：通过切换 r/w 事件 handler 改变阶段
  - connect → 发送请求 → 读取响应头 → 处理响应体
  - 超时管理（connect / send / read）
  - 连接 keepalive 缓存

协议模块（proxy、fastcgi...）:
  - create_request():    构造发往后端的协议请求
  - process_header():    解析后端响应头
  - finalize_request():  请求结束时清理（可选）
```

### 7.2 隐式状态机（通过事件 handler 切换）

nginx 的核心技巧：**不维护显式 state 变量，而是切换事件回调函数指针**：

```
阶段                 write_handler                        read_handler
──────────────────────────────────────────────────────────────────
CONNECT             ngx_http_upstream_send_request       ngx_http_upstream_process_header
SEND (发送请求)      ngx_http_upstream_send_request       (同上)
SEND 完成后          ngx_http_upstream_dummy_handler     (同上)
READ (读响应头)      (同上)                               ngx_http_upstream_process_header
READ (读响应体)      ngx_http_upstream_send_response     ngx_http_upstream_process_body
```

epoll 事件就绪时，调用对应 handler。handler 内部推进状态，
完成后**换函数指针**指向下一阶段的 handler。

### 7.3 核心回调（协议模块实现）

| 回调 | 调用时机 | 职责 |
|------|---------|------|
| `create_request(r, u)` | Send 阶段开始 | 构造发往后端的请求 buffer，放入 `u->request_bufs` |
| `process_header(r, u)` | 收到后端响应数据 | 解析响应头，返回 OK（头部完整）或 AGAIN（继续等数据） |
| `finalize_request(r, rc)` | upstream 结束 | 清理协议特定资源 |

### 7.4 流程

```
rps_http_proxy_handler(r):               // 代理本身极简
  r->keepalive = 0
  u = rps_upstream_create(r, &proxy_callbacks)
  u->peer_addr = plcf->addr
  u->timeouts  = { connect, send, read }
  r->upstream  = u
  rps_upstream_init(u)
  return RPS_AGAIN

rps_upstream_init(u):
  1. u->create_request(r, u)            // 调用协议回调，构造请求
  2. rps_upstream_connect(u)            // 获取连接（keepalive 缓存）
     ├── 命中缓存 → 直接复用
     └── 未命中   → 新建 + 非阻塞 connect
  3. add_event(peer->write, WRITE)
      write_handler = upstream_send_request     // 事件驱动状态机启动
      read_handler  = upstream_process_header
  4. 返回

upstream_send_request(ev):
  1. 检查 SO_ERROR（首次进入时，即 connect 完成）
  2. send(u->request_bufs)
     EAGAIN → return（下次写事件继续）
     全部发完 → write_handler = dummy_handler（不再写）

upstream_process_header(ev):
  1. recv() → u->buffer
  2. u->process_header(r, u)            // 调用协议回调逐次解析
     AGAIN → return（等更多数据）
     OK    → 头部完成
  3. write_handler = upstream_send_response
     read_handler  = upstream_process_body
  4. upstream_send_response(r, u)       // 发响应状态行+头给客户端

upstream_process_body(ev):
  recv() → u->buffer
  → 直接 send 给客户端（non-buffering 模式）
  n == 0 → upstream_finalize(r, OK)

upstream_finalize(r, rc):
  1. 清理 peer 连接（keepalive? 放回缓存 : free）
  2. u->finalize_request(r, rc)         // 协议回调
  3. finalize_request(r, rc)            // 结束请求
  4. complete_request(c)                // 连接收尾
```

### 7.5 与现有代理代码的区别

| 现有 | 新设计 |
|------|--------|
| 代理模块写全部事件回调 | 事件回调在 upstream 层，代理只写 create_request / process_header |
| 显式 state 变量（proxy_state=1,2,3） | 隐式状态，通过换 handler 函数指针 |
| 代理模块管理定时器 | upstream 层统一管理 |
| 代理模块直接 free(uc) | upstream 层通过 keepalive 缓存管理 |

### 7.6 keepalive 缓存

见第 9 章。

---

## 8. 资源归属

| 资源 | 创建点 | 销毁点 |
|------|--------|--------|
| c (含 read/write 事件) | 进程初始化 | 进程退出 |
| c->pool | rps_get_connection | close_connection |
| r | create_request (在 r->pool 中) | finalize → destroy_pool(r->pool) |
| r->pool | create_request | finalize → destroy_pool |
| u (upstream ctx) | rps_upstream_create (在 r->pool 中) | upstream_finalize（内部） |
| u->peer (上游连接) | upstream_connect（keepalive 或新建） | upstream_finalize（释放或回缓存） |
| u->peer->pool | rps_get_connection | free(peer) 或 回缓存保留 |

---

## 9. upstream keepalive

### 9.1 设计目标

upstream 层（第 7 章）在 `upstream_connect` 中自动查缓存，在
`upstream_finalize` 中自动归还。代理模块无需关心缓存逻辑。

```
rps_upstream_connect(u):
  peer = upstream_cache_lookup(u->peer_addr)
  if peer != NULL  → 复用
  else            → rps_get_connection() + connect()

rps_upstream_finalize(r, rc):
  if 可复用 → upstream_cache_push(peer, u->peer_addr)   // 放回，设空闲定时器
  else      → rps_free_connection(peer)                 // 直接关闭
```

### 9.2 缓存结构

每个 upstream 地址（IP:Port）维护一个空闲连接栈（LIFO）：

```
每个 worker 进程内:

upstream_cache: 以 sockaddr 为 key 的哈希表（小规模直接用链表即可）
  └── per-addr 连接栈:
        ├── max_cached:  每个地址最多缓存 N 个空闲连接
        ├── idle_timeout: 空闲超时（默认 60s）
        └── stack:        空闲连接链表（后进先出）
```

每个缓存的连接挂一个空闲定时器，到期关闭。

### 9.3 接口

```
// 获取一个到指定地址的上游连接（先查缓存，查不到则新建 + connect）
rps_connection_t *rps_upstream_get(rps_upstream_addr_t *addr,
                                    rps_cycle_t *cycle,
                                    rps_log_t *log);

// 归还上游连接：可复用则放回缓存（设空闲定时器），否则 close
void rps_upstream_release(rps_connection_t *uc);
```

`rps_upstream_get` 返回的 `uc` 已经 connect 完成（非阻塞），调用者直接 `add_event(uc->write)` 开始发送。

`rps_upstream_release` 内部判断：
- 响应头是 `Connection: close` → 不缓存，直接 free
- 连接出错（SO_ERROR 非零）→ 不缓存
- 请求数超过 `max_requests` → 不缓存
- 否则 → 放入栈顶，设空闲定时器

### 9.4 生命周期

```
代理发起:
  uc = rps_upstream_get(addr)
    ├── 命中缓存 → 直接复用（fd 已连接，跳过 TCP 握手）
    └── 未命中   → rps_get_connection() + connect() 新建

代理请求处理完毕:
  rps_upstream_release(uc)
    ├── 可缓存 → 放入栈顶，设空闲超时
    └── 不可缓存 → rps_free_connection(uc)

空闲超时:
  定时器触发 → 从缓存中移除 → rps_free_connection(uc)
```

### 9.5 与现有代理模块的接入

代理 handler 中改动极小——只换获取和释放方式：

```
rps_http_proxy_handler(r):
  uc = rps_upstream_get(&addr, r->cycle, r->cycle->log);
  // ... 后续与现在相同: r->upstream = uc, add_event(uc->write) ...

代理读回调 n==0:
  r->upstream = NULL;
  rps_upstream_release(uc);  // 替代 rps_free_connection(uc)
  finalize(r, OK);
  complete_request(c);
```

### 9.6 缓存清理

- worker 退出时遍历所有缓存栈，close 所有空闲连接
- 后期可扩展：按内存压力驱逐最冷端（LRU 而非纯栈）

---

## 10. response filter chain

### 10.1 设计目标

当前 handler 需要记住：先调 `rps_http_set_content_length`，再调 `send_header`，
再调 `send_body`。这些应该由框架自动处理。

nginx 的做法是 filter 链：handler 只产出响应头和 body，
后续由一组 filter 依次处理，最终到达 write filter 写入 socket。

### 10.2 filter 链结构

```
handler:
  设置 r->headers_out.status / content_type 等
  r->out_chain = 响应 body 的 buffer 链
  return RPS_OK

↓

header filter:
  1. 自动补 Content-Type（默认 text/html）
  2. 补 Content-Length（从 out_chain 计算总长）
  3. 补 Connection / Keep-Alive
  4. 序列化 headers_out → 放入 out_chain 头部

↓

body filter（可选，当前跳过）:
  chunked 编码 / gzip / range 处理

↓

write filter:
  遍历 out_chain，逐段 send() 到 c->fd
  EAGAIN → 注册 c->write 事件，下次继续
  全部写完 → 返回
```

### 10.3 接口

```
// handler 调用：发送完整响应
rps_int_t rps_http_send_response(rps_http_request_t *r);

// internal: filter 函数签名
typedef rps_int_t (*rps_http_filter_pt)(rps_http_request_t *r);
```

`rps_http_send_response(r)`:
1. 调用 header filter → 序列化 headers_out，计算 Content-Length，补 Connection/Keep-Alive
2. 调用 body filter（当前为空，直接透传）
3. 调用 write filter → 遍历 out_chain，send 到客户端
4. 全部成功 → RPS_OK / EAGAIN → RPS_AGAIN

### 10.4 handler 的用法

```
// handler 设置响应
r->headers_out.status.value = rps_string("200 OK");
r->headers_out.content_type.value = rps_string("text/html");

// 设置 body
rps_buf_t *body = rps_buf_create(r->pool, 256);
body->last = rps_cpymem(body->last, "Hello!", 6);

rps_chain_t *cl = rps_palloc(r->pool, sizeof(rps_chain_t));
cl->buf = body; cl->next = NULL;
r->out_chain = cl;

// 发出去——header filter 自动补 Content-Length: 6
return rps_http_send_response(r);
```

handler 不再单独调 `send_header` / `send_body` / `set_content_length`。
所有这些由 `send_response` → filter 链统一处理。

### 10.5 header filter 自动补全规则

| 头 | 条件 | 动作 |
|----|------|------|
| Content-Type | 未设置 | 默认 `text/html` |
| Content-Length | 未设置且有 out_chain | 遍历 chain 计算总长并设置 |
| Connection | keepalive | 设为 `keep-alive` |
| Keep-Alive | keepalive | `timeout=60` |
| Connection | 非 keepalive | 设为 `close` |
| Server | 未设置 | 默认 `RPS` |

### 10.6 write filter EAGAIN 处理

```
write filter 初次调用: send() → 部分写入
  → 剩余数据留在 out_chain（pos 前移）
  → 注册 c->write 事件，handler = write_filter_continue
  → 返回 RPS_AGAIN

c->write 就绪 → write_filter_continue:
  → 从上次位置继续 send()
  → 全部写完 → del_event(c->write)
  → 如果 r->done → finalize(r) + complete_request(c)
```

这解决了当前 `output_filter` 返回 `RPS_AGAIN` 后无人续传的问题。
