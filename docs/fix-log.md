# RPS 项目修复日志

## 修复日期：2026-05-24

---

## 一、阻断性问题

### 1. `rps_master_process_cycle` 被注释 → `src/core/rps.c`

**问题**：`main()` 末尾的 `rps_master_process_cycle(cycle)` 被注释，服务打印完配置就退出，整个服务器不运行。

**修复**：取消注释，恢复了完整启动流程。

### 2. `rps_open_listening_sockets` 从未调用 → `src/core/rps.c`

**问题**：`rps_http_core_postconfiguration()` 在配置解析阶段把 `listening` 项推进 `cycle->listening` 数组，但没有任何地方调用 `rps_open_listening_sockets(cycle)` 执行 `socket()` + `bind()` + `listen()`。

**修复**：在 `rps_worker_process_cycle` 中，epoll 模式分支下，注册 accept 事件之前调用 `rps_open_listening_sockets(cycle)`。

### 3. Worker 无信号处理 → `src/core/rps.c`

**问题**：`rps_worker_process_cycle` 的事件循环中检查 `rps_terminate` / `rps_quit` 标志，但从未调用 `sigaction()` 注册 handler。信号只在 master 进程注册。

**修复**：在 `rps_worker_process_cycle` 入口注册了 SIGINT / SIGQUIT 信号处理。

### 4. 缺少头文件 → `src/core/rps.c`

**问题**：`rps_daemon()` 使用 `open()`、`O_RDWR`、`dup2()`、`STDIN_FILENO` 等需要 `<fcntl.h>`、`<sys/stat.h>`。

**修复**：添加了 `#include <sys/stat.h>` 和 `#include <fcntl.h>`。

---

## 二、严重 Bug

### 5. 赋值当比较 → `src/event/modules/rps_event_core.c:119`

**问题**：`if(cycle->connections[i].sockaddr = NULL)` 使用了赋值 `=` 而非比较 `==`，导致最后一个连接的 sockaddr 没有被分配。

**修复**：改为 `if(cycle->connections[i].sockaddr == NULL)`。（当前代码已经修复）

### 6. HTTP header 名称用下划线 → `src/http/rps_http_parse.c`

**问题**：多处使用下划线版本的 header 名称（`user_agent`、`content_type`、`content_length`），但真实 HTTP header 使用连字符（`user-agent`、`content-type`、`content-length`）。`rps_str_lowercase` 只转小写不转换分隔符，导致永远匹配不到真实客户端的这些关键 header。

**修复**：
- `rps_http_create_request` 中初始化 key 从 `"user_agent"` → `"user-agent"`，`"content_type"` → `"content-type"`，`"content_length"` → `"content-length"`
- `rps_http_parse_headers` 中的比较字符串同样修改
- `rps_http_parse_headers` 中 `"conntent_type"` → `"content-type"`（修拼写错误）

### 7. Location 继承 srv_conf 数组错误 → `src/http/modules/rps_http_core_module.c:229`

**问题**：`new_loc->srv_conf = srv_container->loc_conf` — location 继承 server 配置时，错误地指向了 `loc_conf` 而非 `srv_conf`。

**修复**：改为 `new_loc->srv_conf = srv_container->srv_conf`。

---

## 三、位运算优先级

### 8. `&` 和 `==` 运算符优先级 → `src/core/rps_conf_file.c`

**问题**（3 处）：`cmd->type & RPS_CONF_BLOCK == 0` 被解析为 `cmd->type & (RPS_CONF_BLOCK == 0)` 而非 `(cmd->type & RPS_CONF_BLOCK) == 0`。

- 第 268 行：`cmd->type & RPS_CONF_BLOCK == 0` — 块指令检查
- 第 358 行：`cmd->type & RPS_CONF_BLOCK != 0` — HTTP_MAIN 块指令判断
- 第 378 行：`cmd->type & RPS_CONF_BLOCK != 0` — HTTP_SRV 块指令判断

**修复**：三处都加了括号 `(cmd->type & RPS_CONF_BLOCK)`。（当前代码已经修复）

---

## 四、Epoll 事件切换 Bug

### 9. EPOLL_CTL_MOD 覆盖旧事件 → `src/event/modules/rps_epoll_module.c`

**问题**：先注册读事件，再注册写事件时，`EPOLL_CTL_MOD` 会用新的 `ee.events`（只有 EPOLLOUT）覆盖旧的（EPOLLIN），导致读事件监听丢失。

**修复**：
- 在 `rps_event_t` 中新增 `epoll_events` 字段追踪当前注册的事件标志
- `rps_epoll_add_event`：使用 EPOLL_CTL_MOD 时，`ee.events |= ev->epoll_events` 合并已有事件
- `rps_epoll_del_event`：删除时清零 `ev->epoll_events`

---

## 五、Proxy 模块修复

### 10. 固定 header 不转发 → `src/http/modules/rps_http_proxy_module.c`

**问题**：`proxy_send_request` 只转发通用 header 链表中的未知 header，但 `Host`、`User-Agent`、`Content-Type`、`Content-Length`、`Connection` 被解析到 `headers_in` 固定字段后没有推入链表。

**修复**：在转发通用 header 之前，显式检查并转发 `User-Agent`、`Content-Type`、`Content-Length` 固定字段（`Host` 已在前面手动添加）。

### 11. 请求体不转发 → `src/http/modules/rps_http_proxy_module.c`

**问题**：`proxy_pass_request_body` 配置项已定义，但 handler 完全不做 body 转发。

**修复**：在 `proxy_send_request` 末尾检查 `pass_request_body` 标志，将请求体数据 `send()` 到 upstream。

### 12. 只读一次 recv（响应截断） → `src/http/modules/rps_http_proxy_module.c`

**问题**：`rps_http_proxy_read_response` 只调用一次 `recv()`，后端响应可能分多个 TCP 段到达。

**修复**：改为循环 `recv()` 直到收到完整的 `\r\n\r\n` header 分隔符，然后一起转发。

---

## 六、Keepalive 支持

### 13. Keepalive 判断缺失 → `src/http/rps_http_parse.c`

**问题**：`r->keepalive` 初始化为 1，但未根据 HTTP 版本和 `Connection` 头做判断。

**修复**：在 `rps_http_parse_headers` 中 header 解析完毕后：
- HTTP/1.0 默认关闭 keepalive，`Connection: keep-alive` 时开启
- HTTP/1.1 默认开启 keepalive，`Connection: close` 时关闭

### 14. finalize_request 不支持 keepalive → `src/http/rps_http_response.c`

**问题**：`rps_http_finalize_request` 无条件销毁 pool 关闭连接。

**修复（第一版）**：keepalive 路径下原地重置 buffer、parse_status、headers 等字段，不销毁 pool，由调用方重新注册读事件。

**修正**：原地重置无法释放 pool 中已分配的 header 链表等内存，多轮 keepalive 导致 pool 无限膨胀。最终改为 keepalive 分支仅 `return`，不做任何清理，由 `done:` 标签统一销毁 pool 并重建 request（见条目 19）。

### 15. Keepalive 重新注册读事件 → `src/core/rps.c`

**问题**：finalize 之后没有重新监听读事件。

**修复**：在 `rps_http_wait_request_handler` 末尾增加 `done` 标签，检查 `!c->close` 时重新 `add_event(READ)`。keepalive 路径下销毁旧 request/pool → 重建 pool → 新建 request → 注册读事件。

---

## 七、缺失的初始化逻辑

### 16. `rps_init_modules` 未调用 → `src/core/rps.c`

**问题**：`rps_module.c` 中定义了 `rps_init_modules(cycle)` 用于调用各模块的 `init_module` 钩子，但从未被调用。

**修复**：在 `rps_master_process_cycle` 中 daemon 化之后、fork 之前调用 `rps_init_modules(cycle)`。

### 17. merge_conf 从未调用 → `src/http/modules/rps_http_core_module.c`

**问题**：`merge_srv_conf` / `merge_loc_conf` 钩子已定义但从未调用，server 不继承 main 配置，location 不继承 server 配置。

**修复**：在 `rps_http_core_postconfiguration` 中，阶段引擎初始化之前，遍历所有 server 和 location，调用各 HTTP 模块的 merge 钩子。

---

## 八、请求体读取

### 18. POST/PUT 请求体不读取 → `src/core/rps.c`

**问题**：解析完 header 后直接进入阶段引擎，body 数据被忽略。

**修复（第一版）**：引入 `reading_body` 标志 + `request_body_rest` 计数器，在 `parse_status == 2` 分支检查 body 是否完整，不够则返回等待下次 `recv`。body 读完后再进入阶段引擎。

**修正（见条目 20）**：`reading_body` 分支中的 `b->pos = b->last` 破坏了 header 结束位置，导致死循环。

---

## 九、请求体读取修正 & Keepalive 修正 & PID/信号完善（同日第二次修复）

### 19. Keepalive 不复用旧 request → `src/core/rps.c` + `src/http/rps_http_response.c`

**问题**：`done:` 标签中 keepalive 直接复用旧 request（`rps_http_finalize_request` 原地重置字段），但 pool 中已分配的 header 链表等内存无法释放，多轮 keepalive 导致 pool 无限膨胀。

**修复**：
- `rps_http_finalize_request`：keepalive 分支删掉所有原地重置代码，仅 `return`（交由调用者处理）
- `rps_http_wait_request_handler` 的 `done:` 标签：keepalive 时 `rps_http_close_request(r)` 销毁旧 pool，然后 `rps_create_pool` + `rps_http_create_request` 全新重建

### 20. 请求体读取死循环 → `src/core/rps.c`

**问题**：`reading_body` 分支在 body 未读完时执行 `b->pos = b->last`，导致 `b->pos`（header 末尾）被推进到 body 中间。body 读完后 fall through 到 `parse_status == 2`，用错误的 `b->pos` 重新计算 `hdr_end`，`have < total` 永远为真，再次设置 `reading_body = 1` 但客户端不会再发数据 → 死循环/连接挂死。

**修复**：彻底删除 `reading_body` 分支。`parse_status == 2` 中 body 不够时直接 `return`，不修改 `b->pos`。下次 `recv` 新数据后 `b->pos` 仍在 header 末尾，`hdr_end` 计算始终正确。

### 21. PID 文件完善 → `src/core/rps.c`

**问题**：PID 文件打开无 `O_CREAT`（文件不存在会失败），无文件锁（无法防多实例），无 `ftruncate`（旧内容残留），退出时不清理。

**修复**：
- `open` 加上 `O_CREAT | O_TRUNC` 和权限 `0644`
- `fcntl(F_SETLK)` 对整个文件加写锁，锁失败打印 "another instance running?" 并退出
- `ftruncate` + 写入 `"<pid>\n"`，全程错误处理
- master 退出时 `unlink` 清理 PID 文件

### 22. 信号机制完善 → `src/core/rps.c`

**问题**：
- 不支持 `SIGTERM`（`kill` 默认信号），daemon 模式无法正常停止
- master 收到信号后只向 worker 转发，不等待退出，不清理资源
- worker 卡死时 master 永远不退出

**修复**：
- 新增 `SIGTERM`：全局标志 `rps_term`、handler 分支、master 注册+阻塞、worker 注册+事件循环检查
- master 关闭流程改为三步：①向所有 worker 转发信号 ②轮询 `waitpid(WNOHANG)` 最多 3 秒 ③超时 worker 发 `SIGKILL` 强杀并打 WARN 日志
- master 退出前 `unlink` PID 文件

---

## 十、未解决 / 已知限制

以下问题已在代码中标记为 TODO，需要后续版本解决：

1. **Proxy 同步阻塞**：`rps_http_proxy_handler` 是同步的（connect + send + recv 一次性完成），在 epoll 非阻塞模式下会阻塞 worker。需要实现 `r->proxy_state` 状态机异步驱动。

2. **Proxy 部分写入**：`send()` / `recv()` 遇到 EAGAIN 时需要注册写/读事件重试。

3. **后端连接复用**：每次请求都新建到 upstream 的连接，需要实现 upstream keepalive 连接池。

4. **响应解析**：当前直接把后端原始响应转发给客户端，不解析后端状态行和响应头。

5. **定时器实现**：`rps_event_find_timer` / `rps_event_expire_timers` 仍为空壳。
