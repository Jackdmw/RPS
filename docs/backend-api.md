# 测试后端 API 文档

FastAPI 后端，默认监听 `127.0.0.1:9090`：

```sh
pip install fastapi uvicorn websockets
python3 test/backend.py 9090
```

---

## HTTP 接口

### GET /
返回固定字符串，验证基本转发。

```
GET / → 200 "Hello from backend"
```

### /* /echo
回显完整请求：请求行 + 所有 header + body。支持 GET/POST/PUT/DELETE。

```
POST /echo
Content-Type: application/json
X-Custom: test

{"key": "value"}

→ 200
POST /echo HTTP/1.1
host: 127.0.0.1:9090
content-type: application/json
x-custom: test
content-length: 16

{"key": "value"}
```

### GET /headers
仅返回后端收到的请求头列表，不含 body。

```
GET /headers → 200
host: 127.0.0.1:9090
x-real-ip: 127.0.0.1
x-forwarded-for: 127.0.0.1
```

### GET /ip
返回客户端 IP 信息，用于验证 `X-Real-IP` 透传和 `X-Forwarded-For` 追加。

```
GET /ip → 200
X-Real-IP: 1.2.3.4
X-Forwarded-For: 10.0.0.1, 127.0.0.1
Remote: 127.0.0.1:12345
```

| 字段 | 说明 |
|------|------|
| `X-Real-IP` | 客户端设置的原始 IP（存在时输出） |
| `X-Forwarded-For` | 代理链 IP 列表（存在时输出） |
| `Remote` | 后端直接看到的连接 IP:Port |

### GET /status/{code}
返回指定的 HTTP 状态码。

```
GET /status/404 → 404 "Not Found"
GET /status/500 → 500 "Internal Server Error"
GET /status/302 → 302 "Found"
```

支持的状态码：200, 301, 302, 400, 403, 404, 500, 502, 503。

### GET /large?size=N
返回指定字节数的大响应体，默认 8192。用于测试缓冲和大数据转发。

```
GET /large?size=32768 → 200, body 为 32768 个 'X'
```

### GET /slow?delay=N
延迟 N 秒后响应，默认 2 秒。用于测试超时。

```
GET /slow?delay=65 → 200 (65 秒后) "Delayed 65s"
```

### GET /chunked
分块传输 10 个 chunk，每个间隔 100ms。

```
GET /chunked → 200, Transfer-Encoding: chunked
chunk 0
chunk 1
...
chunk 9
```

---

## WebSocket

### WS /ws
Echo 服务。收到 text 回显 `"echo: {text}"`，收到 binary 原样返回。

```
connect → 101 Switching Protocols
send "hello" → recv "echo: hello"
send \x01\x02\x03 → recv \x01\x02\x03
```

- 支持 text (opcode 0x1) 和 binary (opcode 0x2)
- 客户端断开时自动清理

---

## 隐藏路由

这两个路由 `include_in_schema=False`，不在 OpenAPI 文档中暴露，用于测试路由隐藏。

### GET /hidden/secret
固定文本响应。

```
GET /hidden/secret → 200 "You found the secret!"
```

### GET /hidden/admin
返回 Host + X-Forwarded-For，模拟内部管理接口。

```
GET /hidden/admin → 200
Admin panel
Host: backend:9090
X-Forwarded-For: 1.2.3.4, 10.0.0.1
```

---

## HTTP 响应头特征

| 场景 | Connection | Content-Length | 备注 |
|------|-----------|----------------|------|
| HTTP/1.1 普通请求 | `keep-alive` | 有 | uvicorn 默认 |
| 客户端带 `Connection: close` | `close` | 有 | |
| WS 升级 | `Upgrade` | 无 | 101 响应 |

body 超过一定大小时 uvicorn 会用 chunked 传输而非 Content-Length。测试时优先用较小 response（`/`、`/echo`）验证基础链路，用 `/large` 验证缓冲。

---

## 启动

```sh
# HTTP + WS 同一端口
python3 test/backend.py 9090
```
