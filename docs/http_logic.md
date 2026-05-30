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

## 7. 代理路径

代理是 CONTENT 阶段的 handler，整体流程:

### 7.1 发起

```
rps_http_proxy_handler(r):
  r->keepalive = 0                     // 代理连接不复用
  获取上游连接 uc
  非阻塞 connect
  build_request(r, uc->pool) → 请求 buffer
  r->upstream = uc
  add_event(uc->write) + add_timer(uc->write, connect_timeout)
  return RPS_AGAIN
```

### 7.2 上游写 (connect → send)

```
state 1: del_timer(connect), 检查 SO_ERROR
         add_timer(write, send_timeout), state=2

state 2: send(headers+body)
         EAGAIN → return
         完成 → del_timer, del_event(write)
                add_event(read) + add_timer(read, read_timeout)
```

### 7.3 上游读

```
读后端数据 → send 给客户端
n==0 (后端关闭):
  del_timer + del_event(read)
  r->upstream = NULL     // 防 finalize 二次释放 uc
  free(uc)
  finalize(r, OK)        // 销毁 r
  complete_request(c)    // 代理 keepalive=0 → c->close=1 → free(c)
```

代理的异步回调直接调 finalize + complete_request，不经过阶段引擎。

### 7.4 代理异常

**前置错误** (uc==NULL, connect 失败等):
  handler 未调 cleanup，r 有效。
  → send_header(502), return RPS_OK
  → checker 调 finalize(r, OK) + complete_request(c)

**add_event 失败 / 写/读回调中的错误**:
  cleanup 内部已调 finalize(r, ERROR) + complete_request(c)
  → handler **必须 return RPS_AGAIN**（checker 不应再调 finalize）

---

## 8. 资源归属

| 资源 | 创建点 | 销毁点 |
|------|--------|--------|
| c (含 read/write 事件) | 进程初始化 | 进程退出 |
| c->pool | rps_get_connection | close_connection |
| r | create_request (在 r->pool 中) | finalize → destroy_pool(r->pool) |
| r->pool | create_request | finalize → destroy_pool |
| uc (代理) | rps_get_connection | read_handler 或 cleanup |
| uc->pool | rps_get_connection | free(uc) |

---

## 9. 待实现

1. output_filter 写事件续传
2. chunked transfer encoding
3. 客户端超时可配置
4. proxy upstream keepalive
5. error_page / access_log
