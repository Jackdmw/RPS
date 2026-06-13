# WebSocket 代理实现设计

## 一、协议规范要点 (RFC 6455)

### 1.1 握手

WebSocket 连接以 HTTP Upgrade 开始：

**客户端请求：**
```
GET /ws HTTP/1.1
Host: example.com
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
```

- `Upgrade: websocket` — 必须
- `Connection: Upgrade` — 必须
- `Sec-WebSocket-Key` — 16 字节随机数 base64 编码
- `Sec-WebSocket-Version` — 固定 13

**服务端 101 响应：**
```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

`Sec-WebSocket-Accept` 算法：
```
SHA1(Base64Decode(Sec-WebSocket-Key) || "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") → Base64
```

### 1.2 数据帧格式

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |  Extended payload (if 126/127) |
|I|S|S|S|  (4)  |A|     (7)     |                               |
|N|V|V|V|       |S|             |                               |
+-+-+-+-+-------+-+-------------+-------------------------------+
|            Masking-key (4 bytes, only if MASK=1)              |
+---------------------------------------------------------------+
|           Payload Data (masked if MASK=1)                     |
+---------------------------------------------------------------+
```

| 字段 | 说明 |
|------|------|
| FIN | 1 = 最后一帧 |
| RSV1-3 | 扩展保留位（压缩等） |
| Opcode | `0x1`=text, `0x2`=binary, `0x8`=close, `0x9`=ping, `0xA`=pong |
| MASK | 客户端→服务端**必须**置 1（防止缓存投毒攻击） |
| Payload Length | ≤125 直接；=126=2字节扩展；=127=8字节扩展 |

Payload 长度编码（7 位 → 扩展 16/64 位，网络字节序）：

| 7-bit 值 | 含义 |
|-----------|------|
| 0–125 | 实际 payload 长度 |
| 126 | 后 2 字节为 16-bit 长度 |
| 127 | 后 8 字节为 64-bit 长度 |

### 1.3 Masking（客户端→服务端）

客户端→服务端的帧 MUST 带 MASK bit + 4 字节 masking key。Payload 每字节与 key 循环 XOR：

```
unmasked[i] = masked[i] ^ masking_key[i % 4]
```

服务端→客户端**禁止** masking。

### 1.4 控制帧

| Opcode | 名称 | 说明 |
|--------|------|------|
| `0x8` | Close | 关闭握手，payload 前 2 字节为状态码 |
| `0x9` | Ping | 心跳检测，接收方必须回 Pong |
| `0xA` | Pong | 心跳响应，payload 必须与 Ping 一致 |

- 控制帧不可分片，payload 必须 ≤125 bytes
- Close 帧交换完毕 → TCP 关闭

---

## 二、透明代理策略

代理不需要解析帧内容，只需**透传**。协议感知只到帧边界级别：

```
客户端 ←──→ [RPS 透明转发] ←──→ 后端
  masked        unmask/remask       unmasked
```

### 2.1 为什么需要解帧边界

直接透传原始字节有两个问题：

1. **TCP 流式传输**：帧可能被 TCP 拆散或合并。需要按帧边界转发以保证帧完整性。
2. **Ping/Pong 拦截**：可由代理自行处理，不转发到对端，减少延迟和带宽。
3. **Close 帧处理**：收到 Close 后需要正确关闭两侧连接。

### 2.2 两种实现深度

| 方案 | 描述 | 复杂度 | 适用场景 |
|------|------|--------|----------|
| **A. 透明代理** | 握手后双向裸转字节，不解析帧 | 极低 | 简单透传 |
| **B. 帧感知代理** | 解析帧头，按帧边界转发，拦截 Ping/Pong | 中等 | 生产环境 |

推荐**方案 B**——复杂度和可靠性取得平衡。

---

## 三、实现方案

### 3.1 整体流程

```
客户端                  RPS                      后端
  │                      │                        │
  │ GET /ws              │                        │
  │ Upgrade: websocket   │                        │
  ├─────────────────────►│                        │
  │                      │  proxy handler 检测    │
  │                      │  Upgrade: websocket    │
  │                      │                        │
  │                      │  GET /ws               │
  │                      │  Upgrade: websocket    │
  │                      ├───────────────────────►│
  │                      │                        │
  │                      │  101 Switching         │
  │                      │  Sec-WebSocket-Accept  │
  │                      │◄───────────────────────┤
  │                      │                        │
  │ 101 Switching        │  (透传)               │
  │ Sec-WebSocket-Accept │                        │
  │◄─────────────────────┤                        │
  │                      │                        │
  ════════════ WebSocket 双向数据 =═══════════════│
  │  Frame  ◄──────────► │  Frame  ◄──────────►  │
  │                      │                        │
```

### 3.2 在现有架构中的切入点

核心思路：复用 upstream 框架的连接管理，替换协议回调。

```
proxy handler (CONTENT phase)
  ├─ 检测 Upgrade: websocket ?
  │   ├─ 否 → 现有 HTTP 代理路径
  │   └─ 是 → WebSocket 代理路径
  │
  └─ WebSocket 路径:
      ├─ 创建 upstream (同 HTTP)
      ├─ create_request  → 构造 WebSocket 升级请求
      ├─ process_header  → 解析 101 响应，验证 Sec-WebSocket-Accept
      └─ 101 响应发送到客户端后:
          ├─ 将客户端 c->read->handler 替换为 ws_client_handler
          ├─ 将后端 u->peer->read->handler 替换为 ws_upstream_handler
          └─ forward_body  ← 握手后无 body，设为 NULL
```

### 3.3 回调实现

#### create_request（构造升级请求）

`ws_create_request(r, u)` — 与 HTTP proxy 类似，差异点：
- **透传** `Upgrade`, `Connection`, `Sec-WebSocket-Key`, `Sec-WebSocket-Version`
- 透传可选的 `Sec-WebSocket-Protocol`, `Sec-WebSocket-Extensions`
- 不写 `Connection: keep-alive`（WebSocket 升级后不需要）
- Host 等通用头照常

```c
static rps_int_t
ws_create_request(rps_http_request_t *r, rps_upstream_t *u)
{
    // 构造 HTTP 升级请求（类似 HTTP proxy 的 create_request）
    // GET /ws HTTP/1.1
    // Host: backend
    // Upgrade: websocket
    // Connection: Upgrade
    // Sec-WebSocket-Key: <client_key>    ← 透传客户端
    // Sec-WebSocket-Version: 13
    // ... 其他透传头 ...
}
```

#### process_header（解析 101 响应）

`ws_process_header(r, u)` — 检查后端是否同意升级：

```c
static rps_int_t
ws_process_header(rps_http_request_t *r, rps_upstream_t *u)
{
    // 1. 解析 status line → 必须是 "101 Switching Protocols"
    // 2. 检查 Upgrade: websocket
    // 3. 检查 Connection: Upgrade
    // 4. (可选) 验证 Sec-WebSocket-Accept
    // 5. 透传响应头给客户端
    // 6. 返回 RPS_OK → 后续进入双向转发模式
}
```

101 不是 101，或者缺少关键头 → 返回 RPS_ERROR → 502。

#### forward_body（双向转发，代替 HTTP body 转发）

握手完成后，`forward_body` 回调不适用。改为**双向事件注册**：

```c
// 在 process_header 返回 OK 后（即 101 已发给客户端）：

// 1. 注册客户端读 → 转发到后端
c->read->handler  = ws_client_to_upstream_handler;
c->read->handler 已注册在 epoll

// 2. 注册后端读 → 转发到客户端
u->peer->read->handler = ws_upstream_to_client_handler;
// u->peer->read 已在 epoll（read_handler 中注册的）
```

#### ws_client_to_upstream_handler

客户端发来 WebSocket 帧 → 直接写到后端连接：

```c
static void
ws_client_to_upstream_handler(rps_event_t *ev)
{
    c = ev->data;
    r = c->data;  // WS context
    
    n = recv(c->fd, buf, sizeof(buf), 0);
    if (n <= 0) { /* 客户端关闭 → 通知后端关闭 */ }
    
    // 直接发送到后端（WebSocket 帧不经过 HTTP 过滤链）
    send(u->peer->fd, buf, n, 0);
}
```

#### ws_upstream_to_client_handler

后端发来数据 → 直接写到客户端连接：

```c
static void
ws_upstream_to_client_handler(rps_event_t *ev)
{
    peer = ev->data;
    r = peer->data;  // WS context
    
    n = recv(peer->fd, buf, sizeof(buf), 0);
    if (n <= 0) { /* 后端关闭 → 通知客户端关闭 */ }
    
    send(r->connection->fd, buf, n, 0);
}
```

### 3.4 WebSocket 上下文

需要一个轻量上下文结构，在握手完成后替换 `r->upstream` 的角色：

```c
typedef struct {
    rps_http_request_t  *r;           /* 关联的 HTTP 请求 */
    rps_connection_t    *client;      /* 客户端连接 */
    rps_connection_t    *upstream;    /* 后端连接 */
    rps_buf_t           *client_buf;  /* 客户端→后端 缓冲区 */
    rps_buf_t           *upstream_buf;/* 后端→客户端 缓冲区 */
    unsigned             client_closed:1;
    unsigned             upstream_closed:1;
} rps_websocket_ctx_t;
```

### 3.5 连接关闭

WebSocket 的优雅关闭：

1. 一端发 Close 帧（opcode=0x8）
2. 对端收到后回 Close 帧
3. 双方交换完毕 → TCP close

透明代理简单做法：任一端 TCP 关闭（`recv=0`）→ 关闭另一端 `shutdown(SHUT_WR)` → 等另一端也关闭 → 释放资源。

### 3.6 超时

WebSocket 连接可能长时间空闲（数小时）。需要：
- 客户端连接：设置长的读超时（如 3600s）或取消超时
- 后端连接：同上
- 可选的 Ping/Pong 代理层心跳

---

## 四、实现步骤

### Phase 1: 最小可行实现（透明代理）

1. proxy handler 中检测 `Upgrade: websocket`
2. 构造升级请求、接收 101 响应、透传给客户端
3. 握手后切换为双向裸字节转发
4. 任一端关闭 → 关闭另一端

### Phase 2: 帧感知（可选增强）

1. 解析帧头 → 按帧边界转发，避免 TCP 拆包问题
2. 拦截 Ping/Pong → 代理自己响应，不转发
3. Close 帧正确中继

### Phase 3: 高级特性（远期）

1. 子协议协商（`Sec-WebSocket-Protocol` 透传）
2. 扩展协商（`permessage-deflate` 等）
3. 连接数限制、速率限制

---

## 五、与现有代码的关系

| 复用组件 | 复用方式 |
|----------|----------|
| upstream 框架 | 连接管理、connect、事件注册 |
| proxy handler | 同一 CONTENT phase 入口，分支处理 |
| proxy_set_header | 握手头透传使用相同数据结构 |
| location 匹配 | 同一个 `proxy_pass` 指令 |
| 内存池 | request pool 管理 WS 上下文生命周期 |

**不经过的组件**：
- HTTP 响应过滤链（body 是二进制帧，不走 `write_filter`）
- 阶段引擎后续阶段（握手完成后不再触发）

---

## 六、配置示例

```nginx
location /ws {
    proxy_pass http://backend;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "Upgrade";
}
```

检测到 `Upgrade: websocket` 后自动切换为 WS 代理模式，同一 `location` 块可同时支持 HTTP 和 WS 代理。
