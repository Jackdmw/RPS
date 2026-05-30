# 清理点分析

## 生命周期关系

```
rps_get_connection(c)
  └─ c->pool = rps_create_pool(1024)   ← connection pool

rps_http_create_request(c)
  └─ request = rps_pcalloc(c->pool, …)  ← request 在 connection pool 中
  └─ r->pool = c->pool                  ← ⚠️ 共用同一个 pool

proxy_handler:
  └─ uc = rps_get_connection(…)         ← upstream connection, 独立 pool
  └─ buf = rps_palloc(uc->pool, 8192)   ← large alloc，在 uc pool
```

**关键问题**：`r->pool == c->pool`。请求和客户端连接共享同一个内存池。

## 清理函数链

### 1. rps_http_finalize_request
```
rps_http_finalize_request(r, rc)               [rps_http_response.c:111]
  └─ c = r->connection
  └─ c->close = 1
  └─ rps_http_close_request(r)
```

### 2. rps_http_close_request
```
rps_http_close_request(r)                      [rps_http_parse.c:102]
  ├─ pool = r->pool          ← 栈上保存，因为 r 在 pool 中
  ├─ if r->upstream:
  │    rps_free_connection(r->upstream)        ← 释放上游连接
  │    r->upstream = NULL
  ├─ if c && c->data == r:
  │    c->data = NULL
  │    c->pool = NULL        ← ⚠️ 置 NULL 防止外层二次释放
  └─ rps_destroy_pool(pool)  ← 销毁共享 pool（r 自身也在此 pool 中）
```

### 3. rps_close_connection / rps_free_connection
```
rps_close_connection(c)                        [rps_connection.c:98]
  ├─ close(c->fd)
  ├─ rps_destroy_pool(c->pool)  ← 如果 pool 已被 close_request 置 NULL，跳过
  └─ rps_memzero(&c->sockaddr / &c->addr_text)

rps_free_connection(c)                         [rps_connection.c:89]
  └─ rps_close_connection(c)
  └─ 归还到 cycle->free_connection 链表
```

### 4. rps_http_proxy_cleanup (proxy 错误路径)
```
rps_http_proxy_cleanup(r, RPS_ERROR)           [rps_http_proxy_module.c:848]
  ├─ c = r->connection, uc = r->upstream
  ├─ if uc:
  │    r->upstream = NULL        ← 防止 close_request 再释放 uc
  │    rps_event_del_timer(uc->write)
  │    rps_event_del_timer(uc->read)
  │    del_event(uc->write) + del_event(uc->read)
  │    rps_free_connection(uc)   ← 释放上游
  └─ if status != RPS_OK:
       rps_http_finalize_request(r, status)  ← → close_request → destroy pool
       rps_free_connection(c)                ← c->pool 已 NULL，安全
```

## 清理点清单

| 调用点 | 文件:行 | 做了什么 |
|--------|---------|---------|
| `rps_http_wait_request_handler` | rps.c:610-710 | 解析失败→finalize_request；正常→done: close_request + free_connection |
| `rps_http_finalize_request` | rps_http_response.c:111 | 标记 c->close=1 → close_request |
| `rps_http_close_request` | rps_http_parse.c:102 | 释放 upstream → 清空 c->pool → destroy_pool |
| proxy handler 错误 | proxy:585-605 | free_connection(uc) 后返回 DONE |
| proxy write handler | proxy:627-753 | timeout/connect fail/send fail → proxy_cleanup |
| proxy read handler | proxy:793-836 | timeout/recv fail → proxy_cleanup；upstream close → 手动清理 |
| proxy read handler (正常) | proxy:816-822 | NULL upstream → finalize → free uc → free c |
| proxy_cleanup | proxy:848-873 | 清 upstream 事件 → free uc → finalize → free c |
| phase engine | rps_http_phases.c:43-498 | error→finalize_request |

## 崩溃分析

### backtrace
```
rps_http_proxy_upstream_read_handler:818  ← 上游关闭 (n==0)
  → rps_http_finalize_request(r, OK)
    → rps_http_close_request
      → rps_destroy_pool(r->pool)  ← r->pool == c->pool
        → rps_destroy_pool_large(pool->large)
          → free(large->alloc)  ← 💥 double free
```

### 正常清理流程（upstream close path）

```
read_handler:816  r->upstream = NULL         ← 保护 uc 不被 close_request 重复释放
read_handler:818  finalize_request(r, OK)
                    → close_request
                      → destroy_pool(r->pool)  ← 销毁共享 pool
                      → c->pool = NULL
read_handler:821  free_connection(uc)        ← 销毁 uc->pool（含 8192 大块）
read_handler:822  free_connection(c)         ← c->pool 已 NULL，跳过 destroy
```

### 推测根因

`rps_destroy_pool` 中 `pool->large` 链表指向的大块地址已被释放。可能原因：

1. **pool->large 链表损坏**：`rps_palloc_large` 用 `rps_palloc(pool, ...)` 分配 large 结构体——这个结构体在 pool 的 small-block 区域中。如果 small-block 耗尽后扩容，旧 block 中的 large 结构体可能被覆盖。

2. **uc->pool 大块和 r->pool 指向同一内存**：虽然两个 pool 独立，但 `rps_palloc(uc->pool, 8192)` 返回的 `malloc` 指针被 `ctx->send_buf` 持有。如果 `ctx`（分配在 uc->pool 中）被误读，可能导致 send_buf 指针被写入 r->pool 的 large 链表。

3. **连接复用污染**：`rps_get_connection` 从 free list 取连接时，只重置了 `close`、`sent`、`listenling`、`addr_text`，并创建新 pool。但 `sockaddr` 和事件的 `data` 指针可能有残留。

## 建议排查顺序

1. 确认 `rps_get_connection` 是否正确清空了所有字段（特别是 `read->data`、`write->data`、`read->timer_set` 等）
2. 检查 `rps_destroy_pool` → `rps_destroy_pool_large` 的 large 链表是否存在循环引用
3. 检查 `ctx = rps_palloc(uc->pool, ...)` — ctx 和 send_buf 都在 uc->pool 中，uc->pool 被销毁时两者同时释放，但 close_request 不会触及 uc->pool
