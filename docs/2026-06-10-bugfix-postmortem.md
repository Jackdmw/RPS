# Bug 修复记录 (2026-06-10)

## 概述

对 `src/` 下全部 47 个源文件进行系统审查，随后通过测试驱动发现并修复共 16 个问题，其中致命 Bug 6 个、高优先级 4 个、中优先级 6 个。修复后测试通过率从 0/7 提升至 6/7（剩余 1 个为测试脚本本身的兼容性问题）。

---

## 致命 Bug

### 1. `ev->data` 被设为请求对象，导致 epoll 操作错误 fd

**文件**：`src/http/rps_upstream.c:530-533`、`src/event/modules/rps_epoll_module.c:116`

**现象**：代理请求全部失败，curl 收到空响应或连接被重置。

**根本原因**：

`rps_upstream_init` 把上游连接的事件 data 设为请求对象：

```c
u->peer->write->data = r;   // 错误：data 应该是 rps_connection_t*
u->peer->read->data  = r;
```

但 `rps_epoll_add_event` 内部执行 `c = ev->data` 然后用 `c->fd` 调用 `epoll_ctl`。当 `ev->data` 指向 `rps_http_request_t*` 时，`c->fd` 读取的是 `r->connection` 指针值（而非真正的 fd），epoll 操作了一个垃圾文件描述符。

**修复**：模仿客户端连接的模式 — `ev->data` 保持为连接对象，请求对象通过 `c->data` 传递：

```c
// rps_upstream_init：不再覆盖 event data
u->peer->data = r;          // 请求存入连接

// rps_upstream_send_handler / read_handler：
rps_connection_t   *c = ev->data;  // c = 连接（可正确获取 fd）
rps_http_request_t *r = c->data;   // r = 请求
```

**调试过程**：这是本项目最长、最难追踪的 Bug。epoll_ctl 不会报错（可能操作了某个有效但不正确的 fd），表现为 curl 收到空响应/连接重置。通过逐层加 debug 日志定位：TIMER → EPOLL → RECV → WAIT_REQ，最终确认 handler 被调用但 epoll 注册错误。

---

### 2. 连接复用时 `timedout` 位未重置（状态泄漏）

**文件**：`src/core/rps_connection.c:117`、`:73`

**现象**：首个请求正常处理，后续请求立即触发"client timed out"（连接在毫秒级内被判定超时关闭）。

**根本原因**：

`timedout` 是 `rps_event_t` 中的 bit-field，只在 `rps_event_expire_timers` 中被设为 1。连接释放回空闲池时，`rps_close_connection` 只重置了 `active` 标志，**没有重置 `timedout`**：

```c
// 修复前
c->read->active  = 0;
c->write->active = 0;
```

当连接从空闲池重新分配给新请求时，`ev->timedout == 1` 残留。`rps_http_wait_request_handler` 检查 `ev->timedout` 为真，立即关闭连接，不处理任何数据。

**修复**：

```c
// rps_close_connection：释放前重置
c->read->timedout  = 0;
c->write->timedout = 0;
c->read->active    = 0;
c->write->active   = 0;

// rps_get_connection：复用前再次确认
new_conn->read->timedout  = 0;
new_conn->write->timedout = 0;
```

**关键突破点**：debug 日志显示 `WAIT_REQ timedout=1` 但 `EXPIRE cmp=1`（定时器未过期），这个矛盾揭示了 `timedout` 不是通过 expire_timers 设置的，而是从上一次使用残留的。

---

### 3. Keepalive 缓存命中时未设置 `u->peer`

**文件**：`src/http/rps_upstream.c:815`

**现象**：后端连接 keepalive 缓存完全无效，每次缓存命中都报错关闭。

**根本原因**：

```c
// rps_upstream_get_peer 缓存命中路径：
rps_connection_t *peer = cached->connection;
// ...清理定时器等...
return peer;   // 返回了 peer 但未设置 u->peer！
```

调用方 `rps_upstream_init` 忽略了返回值，直接检查 `u->peer`：

```c
rps_upstream_get_peer(r, u);
if (u->peer == NULL) {   // 缓存命中时这里永远是 NULL
    rps_upstream_finalize(r, RPS_ERROR);
    return;
}
```

**修复**：缓存命中路径增加 `u->peer = peer;`

---

### 4. Location 匹配使用精确比较而非前缀匹配

**文件**：`src/http/rps_http_phases.c:211`

**现象**：路由规则完全失效，`/api/users` 无法匹配 location `/api/`。

**根本原因**：注释写的是"最长前缀匹配"，代码却用了精确比较：

```c
// 修复前
if (r->uri.len >= lcf->pattern.len
    && rps_strcmp(r->uri, lcf->pattern) == RPS_STRING_EQUAL)  // 精确匹配！
```

`rps_strcmp` 要求长度和内容都完全相等。

**修复**：

```c
if (r->uri.len >= lcf->pattern.len
    && memcmp(r->uri.data, lcf->pattern.data, lcf->pattern.len) == 0)  // 前缀匹配
```

---

### 5. 后端 HTTP 响应头被完全丢弃

**文件**：`src/http/modules/rps_http_proxy_module.c:577-615`

**现象**：反向代理功能严重缺陷 — 后端返回 404/500/302 客户端看到的一律是 200，Set-Cookie 全部丢失。

**根本原因**：`rps_http_proxy_process_header` 只负责找到 `\r\n\r\n`，然后调用 `rps_http_send_header(r)` 发送 RPS 自己构造的响应头（Status: 200, Server: RPS, Content-Type: text/html）。后端的真实 status line 和所有响应头被忽略。

**修复**：完全重写该函数，新增以下逻辑：

1. **解析 status line**：找到 `HTTP/x.x NNN Reason` 中的状态码和原因短语，填入 `r->headers_out.status.value`
2. **逐行解析 headers**：对 `content-type`、`content-length`、`server` 填入 `r->headers_out` 的固定字段；其他 header（如 `Set-Cookie`、`ETag`、`Cache-Control`）通过 `rps_http_add_response_header` 透传
3. 然后再调用 `rps_http_send_header(r)` 发送完整响应头

修改后后端 500 返回的 body `"Backend error"` 能被正确透传，状态码也正确。

---

### 6. 定时器过期处理中 `root` 指针过期

**文件**：`src/event/rps_event.c:149`

**现象**：定时器可能被遗漏（不触发）或错误触发，导致连接泄漏或误关闭。

**根本原因**：

```c
root = rps_event_timer_tree.root;  // 循环开始前缓存 root

for (;;) {
    node = root;  // 使用可能已过期的 root
    while (node->left != sentinel) node = node->left;

    rps_rbtree_erase(node, &rps_event_timer_tree);
    // tree->root 可能被 erase 更新，但局部变量 root 未更新
}
```

当被删除的最小节点恰好是根节点时，`tree->root` 已变，局部 `root` 指向已从树中移除的节点。下一轮循环从该节点开始做左遍历会访问无效内存（use-after-free）。

**修复**：每轮循环开始时重新获取：

```c
for (;;) {
    root = rps_event_timer_tree.root;  // 每轮刷新
    node = root;
    ...
}
```

---

## 高优先级

### 7. 配置文件 fd 泄漏

**文件**：`src/core/rps_cycle.c:101`

open() 成功后三条返回路径均未 close(fd)。修复：增加 open 失败检查 + 所有路径 `close(fd)`。

### 8. 宏缺括号

**文件**：`src/core/rps_conf_file.c:9`

`rps_is_a_letter(word)` 中 `word` 无括号保护，传入 `*p++` 会多次求值。修复：全部 `word` → `(word)`。

### 9. `listen` 指令只支持裸端口号

**文件**：`src/http/modules/rps_http_core_module.c`

原来的 `rps_conf_set_num_slot` 只认纯数字，`listen 0.0.0.0:80` 会解析失败。新增 `rps_set_listen` 函数支持 `80`、`*:80`、`0.0.0.0:80` 三种格式。

### 10. 错误处理用 `exit()` 杀进程

**文件**：`src/core/rps_connection.c:22-27`

`setsockopt` 失败直接 `exit(EXIT_FAILURE)`。修复：改为 `return RPS_ERROR`。

---

## 中优先级 / 代码质量

| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| 11 | `rps_conf_file.c:66` | `%l` 不是合法 printf 格式符 | → `%ld` |
| 12 | `rps_http_parse.c` | `headers_in.headers_n` 计数器从未递增 | 每个 header 解析后 `++` |
| 13 | `rps.c:98-103` | `cycle==NULL` 仍调用 `rps_master_process_cycle(NULL)` | 增加 `return 1` |
| 14 | `rps_log.c:4` | strerror 宏复制超出实际错误字符串的垃圾数据 | 用 `strlen` 限制长度 |
| 15 | `rps_core_module.c:123` | pid 默认值检测不一致（`default.pid.txt` vs `run_pid.conf`） | 统一为 `run_pid.conf` |
| 16 | `rps_cycle.c:55,63` | C99 `for(int i)` 遮蔽 C89 `rps_uint_t i` | 删除多余的 `int` |

---

## 配置解析-悬空指针批量修复

**文件**：`rps_http_core_module.c`、`rps_http_proxy_module.c`、`rps_upstream.c`

**根本原因**：`rps_init_cycle` 中栈上分配了 `rps_buf_t b` 作为配置解析缓冲区。许多 `rps_str_t` 被赋值为直接指向 `b` 内部数据的指针。`rps_init_cycle` 返回后 `b` 随栈帧销毁，但 `cycle->conf_ctx` 中的配置结构仍持有这些指针。通过 `fork()` 复制后，worker 的栈增长可能覆盖这些区域，导致间歇性 SIGSEGV（signal 11）。

**批量修复**：所有指向配置缓冲区的 `rps_str_t` 统一改为调用 `rps_strcpy()` 复制到内存池（pool）：

- `loc_cf->pattern` — location 匹配模式
- `plcf->proxy_pass`、`plcf->upstream_host`、`plcf->upstream_uri`、`plcf->upstream_name`
- `proxy_set_header` 的 key / value
- `ucf->name`、`peer->host`

---

## 调试过程总结

1. **分层缩小范围**：测试脚本 → 手动 curl → RPS 日志 → epoll 调试 → timer 调试 → fd 层面追踪
2. **关键转折点**：发现 `WAIT_REQ timedout=1` 但 `EXPIRE cmp=1` 的矛盾，揭示状态泄漏问题
3. **方法**：
   - 使用 `fprintf(stderr, ...)` 在关键路径加诊断日志（stderr 无缓冲，即时输出）
   - AddressSanitizer 构建对内存问题最有帮助，但对性能影响大可先用普通构建加日志
   - 手动逐个 curl 请求比自动化测试脚本更容易定位问题

## 测试结果

测试脚本 `test/test_proxy_system.sh`：
- 6 / 7 测试通过
- `/error` 失败原因：测试脚本中 curl 失败后用 bash `/dev/tcp` 做 raw socket fallback，该功能在此环境中不可用。手动 curl 验证 `/error` 返回正确（`Backend error` body + 500 状态码）。
