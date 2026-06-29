# Upstream 逻辑/IO 分层重构

## 问题

`rps_upstream.c` 中的函数混杂了业务逻辑和非阻塞 I/O，线程模式无法复用。

## 拆分原则

每个函数拆成两层：
- **逻辑层**（新函数，`_logic` 后缀）：纯数据操作，不调任何 I/O
- **I/O 层**（原函数）：调逻辑层 + 调 epoll/poll/send/recv

线程模式复用逻辑层，替换 I/O 层。

---

## 1. `rps_upstream_connect` → 已完成

现状：socket 创建 + connect 放在一起，线程已直接复用整函数。

---

## 2. `rps_upstream_init` — 启动 upstream 流程

当前职责：
1. 负载均衡选择 peer → 设置 u->peer_addr
2. 调 create_request 回调 → 构造请求
3. 调 rps_upstream_get_peer → 获取/建立后端连接
4. 注册 write 事件 → 开始发送

拆分：

```c
// 逻辑层（线程可复用）
rps_int_t rps_upstream_prepare(rps_http_request_t *r, rps_upstream_t *u) {
    // 1. select peer
    if (u->upstream_conf) {
        peer = u->upstream_conf->select_peer(u->upstream_conf);
        u->peer_addr.host = peer->host;
        u->peer_addr.port = peer->port;
    }
    // 2. create request
    if (u->create_request && u->create_request(r, u) != RPS_OK) return RPS_ERROR;
    // 3. get peer connection
    rps_upstream_get_peer(r, u);
    if (!u->peer) return RPS_ERROR;
    return RPS_OK;
}

// I/O 层（原函数）
void rps_upstream_init(r, u) {
    if (rps_upstream_prepare(r, u) != RPS_OK) { finalize; return; }
    // 4. register write event (reactor only)
    u->peer->write->handler = rps_upstream_send_handler;
    u->peer->read->handler  = rps_upstream_read_handler;
    u->peer->data = r;
    add_event(u->peer->write, RPS_WRITE_EVENT);
    add_timer(u->peer->write, u->connect_timeout);
}
```

线程使用：调 `rps_upstream_prepare` → 自己 poll-wait connect → 自己 send request。

---

## 3. 请求发送 — 从 `rps_upstream_send_handler` 拆出

当前职责：
1. 首次触发：检查 connect 结果 (SO_ERROR)
2. 发送请求数据
3. 发送完毕：切换到读事件

拆分：

```c
// 逻辑层
rps_int_t rps_upstream_send_request(rps_http_request_t *r, rps_upstream_t *u,
                                     rps_int_t *done) {
    ssize_t n = send(u->peer->fd, ...);
    if (n < 0) return RPS_ERROR;
    u->request_sent += n;
    *done = (u->request_sent >= request_len);
    return RPS_OK;
}
```

I/O 层留在 send_handler 内部。线程模式直接用循环调 `rps_thread_blocking_send_all`。

---

## 4. 响应接收 — 从 `rps_upstream_read_handler` 拆出

当前职责：
1. recv 数据到 response_buf
2. READ_HEADER: 调 process_response 解析+发送头
3. READ_BODY: 累加 body_received，调 write_filter 推送 body
4. CL 完成判定

拆分：

```c
// 逻辑层：处理已读入 response_buf 的数据
rps_int_t rps_upstream_process_data(rps_http_request_t *r, rps_upstream_t *u,
                                     ssize_t n) {
    u->response_buf->last += n;

    if (u->read_state == READ_HEADER) {
        rc = u->process_response(r, u);
        if (rc == RPS_AGAIN) return RPS_AGAIN;  // header incomplete
        if (rc != RPS_OK) return RPS_ERROR;
        u->read_state = READ_BODY;
    } else {
        u->body_received += n;
    }

    // body forwarding + completion check (same as current READ_BODY block)
    ...
}
```

I/O 层：recv 数据 → 调 process_data → 注册事件/返回。

线程模式：recv 数据 → 调 process_data。

---

## 5. 拆分后的调用关系

### Reactor 路径

```
rps_upstream_init          → rps_upstream_prepare + add_event
  └─ send_handler          → 内联 I/O + 切换读
       └─ read_handler     → recv → rps_upstream_process_data
            └─ process_response 回调
            └─ write_filter
            └─ CL complete → finalize
```

### Thread 路径

```
rps_thread_proxy_run:
  create_request 回调          ← 同
  rps_upstream_connect         ← 复用
  rps_thread_poll_wait         ← I/O 替换
  rps_upstream_prepare         ← 新增复用
  rps_thread_blocking_send_all ← I/O 替换
  loop:
    recv                        ← I/O
    process_response 回调       ← 同
    recv                        ← I/O
    write_filter                ← 同
  close(fd)
```

---

## 6. 具体改动清单

| 文件 | 改动 |
|------|------|
| `rps_upstream.h` | 新增 `rps_upstream_prepare`、`rps_upstream_process_data` 声明 |
| `rps_upstream.c` | 从 `rps_upstream_init` 拆出 `rps_upstream_prepare`；从 `read_handler` 拆出 `rps_upstream_process_data` |
| `rps_thread_proxy.c` | `rps_thread_proxy_run` 调 `rps_upstream_prepare` + `rps_upstream_process_data`，去掉重复逻辑 |
