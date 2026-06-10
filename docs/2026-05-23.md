# Bug 修复日志

## 2026-05-23 — rps_http_parse.c 修复

### Bug 1: 错误转义 `\?` → `?`

- **位置**: `rps_http_parse_request_line` 第 154 行
- **问题**: `if (arg.data[i] == '\?')` 中 `\?` 是无效转义，意图是匹配字面量 `?` 字符
- **修复**: 改为 `if (arg.data[i] == '?')`
- **影响**: 修复前 URI 中的 query string (`?a=1`) 不会被拆分出来，`r->args` 始终为空

### Bug 2: HTTP 版本检查缺失 HTTP/1.1

- **位置**: `rps_http_parse_request_line` 第 164 行
- **问题**: 只检查了 HTTP/1.0 和 HTTP/2.0，缺少 HTTP/1.1，且用了裸 `||` 而非显式 `== RPS_STRING_EQUAL`
- **修复**: 补上 HTTP/1.1，统一使用 `== RPS_STRING_EQUAL` 显式比较
- **影响**: 修复前 HTTP/1.1 请求的 `r->http_version` 不会被赋值，保持为空字符串

### Bug 3: 解析成功后未设置 parse_status

- **位置**: `rps_http_parse_request_line` 第 175 行，`rps_http_parse_headers` 第 203 行
- **问题**:
  - `parse_request_line` 成功解析起始行后没有设 `r->parse_status = 1`，导致事件循环不会调用 `parse_headers`
  - `parse_headers` 解析完所有头部后没有设 `r->parse_status = 2` 也没有更新 `buf->pos`，导致不会进入阶段引擎且下次读事件会重复解析
- **修复**:
  - `parse_request_line` 返回前加入 `r->parse_status = 1`
  - `parse_headers` 遇到空行时更新 `buf->pos = pos + 2` 并设 `r->parse_status = 2`
- **影响**: 修复前请求解析完起始行后永远卡住，无法进入 header 解析和阶段引擎

### Bug 4: rps_http_close_request 空壳

- **位置**: `rps_http_close_request` 第 98-100 行
- **问题**: 函数体为空，请求结束时 upstream 连接泄漏、请求内存池未释放
- **修复**: 实现完整清理逻辑 — 关闭 upstream 连接、从 connection 上摘除自己、销毁请求内存池
- **影响**: 修复前每个请求泄漏一个内存池 + upstream 连接，压测会 OOM
