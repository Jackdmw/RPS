# IO 逻辑分离方案 v2

## 核心思路

Proxy 和 Upstream 模块提供了逻辑功能，同时附带事件驱动 IO。只拆 **纯逻辑** 那半步，IO 半步留在原地。

回调机制（`create_request`、`process_response`、`write_continue`）本身就是抽象层——reactor 通过事件调用，线程在 poll 循环中直接调用。

---

## 一、已完成的拆分

### `rps_upstream_init` → `rps_upstream_prepare`  ✓

```c
// 逻辑层
rps_int_t rps_upstream_prepare(r, u) {
    select_peer → set u->peer_addr
    create_request 回调
    get_peer → set u->peer
    return OK/ERROR
}

// IO 层（原地）
rps_upstream_init(r, u) {
    rps_upstream_prepare(r, u)
    // reactor only: 注册 write 事件
}
```

线程用法：调 `prepare` → poll-wait connect → blocking send。

---

## 二、待拆分的函数

### 2.1 `rps_http_proxy_process_response` — HTTP 响应处理

当前代码分两步：

```
第一步（解析，纯逻辑）:
  找 \r\n\r\n
  解析 status line → 设 u->keepalive（HTTP 版本）
  逐行解析 header:
    content-type → r->headers_out
    content-length → u->content_length_n + r->headers_out
    server → r->headers_out
    connection → u->keepalive（覆盖版本默认值）
  其他 header → rps_http_add_response_header
  移动 buf->pos 到 body 起始
  设 body_received = buf 中已有 body 字节数
  设 header_complete = 1

第二步（发送，IO）:
  rps_http_send_header(r) → header_filter + write_filter
  挂 body chain（response_buf → out_chain）
  rps_http_write_filter(r) → 推送已有 body
```

拆出：

```c
// 纯逻辑层
rps_int_t rps_http_proxy_parse_response(rps_http_request_t *r, rps_upstream_t *u)
{
    // 找 \r\n\r\n，没找到返回 RPS_AGAIN
    // 解析 status line、所有 headers
    // 移动 pos，设 body_received、header_complete
    return RPS_OK;
}
```

| 谁 | 怎么用 |
|----|--------|
| reactor `process_response` | parse → send_header → body chain → write_filter |
| thread 代理循环 | parse → blocking send headers → blocking write body |

### 2.2 `ws_process_response` — WS 101 校验

当前代码：

```
找 \r\n\r\n
校验 101 状态码
透传原始 101 字节给客户端（rps_unix_send）
初始化 ws_ctx + 切换 handler
```

拆出：

```c
// 纯逻辑层：校验 101
rps_int_t ws_check_101(rps_upstream_t *u)
{
    // 找 \r\n\r\n，没找到返回 RPS_AGAIN
    // 校验 status line 含 " 101 "，不是返回 RPS_ERROR
    return RPS_OK;
}
```

| 谁 | 怎么用 |
|----|--------|
| reactor `ws_process_response` | check → 透传 raw send → 设 handler |
| thread WS | check → blocking send 101 |

> 注意：`ws_process_response` 内的透传 send 比较简单（一次 `rps_unix_send`），线程版也是阻塞 send。拆出 `ws_check_101` 只是为了不在线程里重复写 101 解析校验。

---

## 三、不需要拆的函数（已有回调抽象覆盖）

| 函数 | 原因 |
|------|------|
| `create_request` (HTTP + WS) | 纯逻辑，已是回调 ✓ |
| `rps_http_write_filter` | 本身是同步函数，两个模式都能直接调用 ✓ |
| `rps_http_header_filter` | 纯逻辑，已公开 ✓ |
| `read_handler` / `send_handler` | IO 骨架，通过回调调用逻辑 ✓ |
| `rps_upstream_connect` | 已复用 ✓ |
| `rps_upstream_get_peer` | 已复用 ✓ |
| `rps_upstream_finalize` | 已复用 ✓ |
| `rps_upstream_free_peer` | 已复用 ✓ |

---

## 四、改动清单

| 文件 | 新增 | 修改 |
|------|------|------|
| `rps_http_proxy_module.c` | `rps_http_proxy_parse_response(r, u)` | `rps_http_proxy_process_response` 调 parse 后只做发送 |
| `rps_http_proxy_module.c` | `ws_check_101(r, u)` | `ws_process_response` 调 check 后只做透传 + handler 切换 |
| `rps_http_proxy_module.h` | 声明 `rps_http_proxy_parse_response` | — |
| `rps_thread_proxy.c` | — | `rps_thread_proxy_run` 回调中调用 `parse_response` |
| `rps_thread_proxy.c` | — | `rps_thread_ws_start` 调 `ws_check_101` |

---

## 五、`rps_thread_proxy_run` 最终形态

```c
rps_int_t rps_thread_proxy_run(r, u) {
    // 1. prepare（复用 upstream 逻辑）
    rps_upstream_prepare(r, u);

    // 2. poll-wait connect（线程 IO）
    poll_wait(u->peer->fd, POLLOUT, timeout);
    check SO_ERROR;

    // 3. blocking send request（线程 IO）
    blocking_send_all(u->peer->fd, request_bufs);

    // 4. 阻塞收响应
    while (!u->header_complete) {
        poll_wait(fd, POLLIN);
        recv → append to response_buf;
        rps_http_proxy_parse_response(r, u);  // ← 复用解析逻辑
    }

    // process_response 已发送 header + 挂 body chain
    // 5. 收 body + 转发
    while (body_received < content_length_n) {
        poll_wait(fd, POLLIN);
        recv → append to response_buf;
        rps_http_write_filter(r);  // ← 复用写过滤链
    }

    close(fd);
}
```

和 reactor 的唯一差异：IO 用 poll + blocking send/recv，逻辑用同样的 parse + write_filter。
