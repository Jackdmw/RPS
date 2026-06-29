# IO 逻辑分离重构方案

## 核心原则

Reactor 和 Thread 的差异只有两点：
1. **I/O 方式**：epoll 非阻塞 vs poll 阻塞
2. **共享资源**：连接池需要互斥锁

除此之外——请求构造、响应解析、header 发送、body 转发、keepalive 管理——全部一致，应该共享同一套代码。

## 一、Upstream 模块拆分

### 1. `rps_upstream_init` → 拆出 `rps_upstream_prepare`

`rps_upstream_init` 当前职责：
- 负载均衡选 peer
- 调 create_request 回调构造请求
- 调 get_peer 获取后端连接
- **注册 write 事件 + 定时器** ← IO

```c
// 逻辑层：线程和 reactor 共用
rps_int_t rps_upstream_prepare(rps_http_request_t *r, rps_upstream_t *u)
{
    // 1. select peer
    if (u->upstream_conf) {
        peer = u->upstream_conf->select_peer(u->upstream_conf);
        if (!peer) return RPS_ERROR;
        u->peer_addr.host = peer->host;
        u->peer_addr.port = peer->port;
    }
    // 2. create request
    if (u->create_request && u->create_request(r, u) != RPS_OK)
        return RPS_ERROR;
    // 3. get peer connection
    rps_upstream_get_peer(r, u);
    return (u->peer != NULL) ? RPS_OK : RPS_ERROR;
}
```

| 调用方 | 用法 |
|--------|------|
| reactor `rps_upstream_init` | prepare → 设 handler → add_event → add_timer |
| thread `rps_thread_proxy_run` | prepare → poll-wait connect → blocking send |

### 2. `rps_upstream_read_handler` → 拆出 `rps_upstream_process_data`

当前 `read_handler` 职责：
- **recv 数据** ← IO
- 调 process_response 解析头
- 累加 body_received，调 write_filter 转发 body
- CL 完成判定，删后端读

实际上 `read_handler` 里的业务逻辑（从 "recv 之后" 开始）已经是一个独立的状态机。直接提取：

```c
// 逻辑层：处理已读入 response_buf 的数据（recv 的数量 n）
rps_int_t rps_upstream_process_data(rps_http_request_t *r, rps_upstream_t *u,
                                     ssize_t n)
{
    u->response_buf->last += n;

    // READ_HEADER: 解析 + 发送头
    if (u->read_state == RPS_UPSTREAM_READ_HEADER) {
        rc = u->process_response(r, u);
        if (rc == RPS_AGAIN) return RPS_AGAIN;  // 头不完整
        if (rc != RPS_OK)   return RPS_ERROR;
        u->read_state = RPS_UPSTREAM_READ_BODY;
    } else {
        u->body_received += n;
    }

    // READ_BODY: 转发 body + CL 判定
    if (u->content_length_n > 0) {
        rc = rps_http_write_filter(r);
        if (rc == RPS_AGAIN) return RPS_AGAIN;  // 客户端写阻塞（reactor 处理）
        if (rc != RPS_OK)   return RPS_ERROR;
        // buf 复位
        if (pos == last) { pos = last = start; }
        // CL 完成
        if (u->body_received >= u->content_length_n) {
            del_backend_read();
            return RPS_DONE;
        }
    } else {
        // 无 CL: header 后无 body
        del_backend_read();
        return RPS_DONE;
    }
    return RPS_OK;
}
```

返回值语义：
| 返回值 | 含义 | reactor 处理 | thread 处理 |
|--------|------|-------------|------------|
| RPS_OK | 继续读 | return（事件保持） | 循环收下一段 |
| RPS_AGAIN | 头不完整/写阻塞 | return / 注册写事件 | 继续循环收 / poll-wait 写 |
| RPS_DONE | 响应完成 | finalize | close fd, 返回上层 |
| RPS_ERROR | 错误 | finalize(ERROR) | close fd, return ERROR |

| 调用方 | 用法 |
|--------|------|
| reactor `read_handler` | `recv` → `process_data` → AGAIN? return : DONE? finalize |
| thread `proxy_run` | `recv` → `process_data` → AGAIN? continue recv : DONE? break |

### 3. `rps_upstream_connect` → 已复用

两个模式都直接调用，无需拆分。

### 4. `rps_upstream_finalize` → 已复用

全部逻辑（清理后端连接、502 错误响应、finalize request + complete）两个模式共用。

---

## 二、Proxy 模块拆分

### 回调已天然分离

proxy 模块通过三个回调注入 upstream：
- `create_request` — 纯逻辑，两个模式共用 ✓
- `process_response` — 纯逻辑 + `rps_http_write_filter`（write_filter 本身是同步的，两个模式都可用） ✓
- `write_continue` — reactor 专用（epoll 回调），线程不需要 ✗

`rps_http_proxy_handler` 里的 IO 分叉已经干净：

```c
if (if_pthread) {
    rps_thread_proxy_run(r, u);    // 线程路径
} else {
    rps_upstream_init(r, u);      // reactor 路径
}
return RPS_AGAIN;
```

### WS 的 `ws_process_response`

当前 `ws_process_response` 内部有 `rps_unix_send`（直接 IO）+ 设置 epoll handler。线程模式需要自己的版本（inline 101 解析+透传），因为线程没有事件结构。这个差异可以接受——WS 的 I/O 模式本来就完全不同（双向裸转），分开处理比强行统一更清晰。

---

## 三、改动清单

| 文件 | 新增 | 修改 |
|------|------|------|
| `rps_upstream.h` | `rps_upstream_prepare()` 声明<br>`rps_upstream_process_data()` 声明 | — |
| `rps_upstream.c` | 实现 `rps_upstream_prepare`<br>实现 `rps_upstream_process_data` | `rps_upstream_init` 调 `prepare` 后只做事件注册<br>`read_handler` 调 `process_data` 后只做 IO 分支 |
| `rps_thread_proxy.c` | — | `rps_thread_proxy_run` 调 `prepare` + poll-wait + 循环 `recv`/`process_data` |

---

## 四、`rps_thread_proxy_run` 重构后伪代码

```c
rps_int_t rps_thread_proxy_run(r, u) {
    // 1. prepare（复用 upstream 逻辑）
    if (rps_upstream_prepare(r, u) != RPS_OK) return RPS_ERROR;

    // 2. poll-wait connect（线程唯一 IO）
    poll_wait(u->peer->fd, POLLOUT, timeout);
    check SO_ERROR;

    // 3. blocking send request（线程 IO）
    blocking_send_all(u->peer->fd, request_bufs);

    // 4. recv + process_data 循环（IO + 复用逻辑）
    u->read_state = READ_HEADER;
    u->response_buf->pos = u->response_buf->last = u->response_buf->start;

    while (1) {
        poll_wait(fd, POLLIN);
        n = recv(fd, buf);
        if (n <= 0) break;

        rc = rps_upstream_process_data(r, u, n);
        if (rc == RPS_DONE) { close(fd); return RPS_HTTP_DONE; }
        if (rc == RPS_ERROR) { close(fd); return RPS_ERROR; }
        // RPS_OK or RPS_AGAIN → continue loop (poll 会阻塞等数据)
    }
    close(fd);
    return RPS_ERROR;
}
```

和 reactor `read_handler` 的核心差异：线程用 `while` + `poll` 阻塞循环替代 epoll 事件驱动。

---

## 五、不变的部分

以下代码完全不动：
- `create_request` 回调（HTTP proxy、WS）
- `process_response` 回调（HTTP proxy）
- `rps_http_write_filter` 及过滤链
- `rps_upstream_connect`
- `rps_upstream_get_peer`
- `rps_upstream_free_peer`
- `rps_upstream_finalize`
- `rps_upstream_new_peer_conn` / `close_peer_conn`
- `rps_http_proxy_handler` 的 if_pthread 分叉
- 所有 keepalive 缓存逻辑
