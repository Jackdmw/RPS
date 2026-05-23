# Proxy 模块实现日志

## 2026-05-23 — rps_http_proxy_module 实现

### 已实现功能

#### 1. 配置指令（8 个）

| 指令 | 类型 | 说明 |
|------|------|------|
| `proxy_pass` | location, TAKE1 | 后端 upstream URL，如 `http://backend:8080/api` |
| `proxy_set_header` | location, TAKE2 | 添加自定义转发 header，如 `Host $proxy_host` |
| `proxy_connect_timeout` | location, TAKE1 | 连接后端超时（毫秒），默认 60000 |
| `proxy_read_timeout` | location, TAKE1 | 读取后端响应超时（毫秒），默认 60000 |
| `proxy_send_timeout` | location, TAKE1 | 发送请求到后端超时（毫秒），默认 60000 |
| `proxy_buffering` | location, TAKE1 | 是否缓冲后端响应，默认 on |
| `proxy_pass_request_headers` | location, TAKE1 | 是否转发客户端请求头，默认 on |
| `proxy_pass_request_body` | location, TAKE1 | 是否转发客户端请求体，默认 on |

#### 2. proxy_pass URL 解析

支持的 URL 格式：
- `http://host`
- `http://host:port`
- `http://host/path`
- `http://host:port/path`

自动解析出 host、port（默认 80）、uri path 并存入配置结构体。

#### 3. 模块注册

- 新增 `rps_http_proxy_module` 到全局模块列表 `rps_modules[]`
- 注册 `create_loc_conf`：分配并初始化 location 级别配置
- 注册 `merge_loc_conf`：支持 server→location 配置继承
- 注册 `postconfiguration`：在 CONTENT_PHASE 注册 proxy handler

#### 4. 代理 handler（CONTENT_PHASE）

执行流程：
1. 检查 `r->loc_conf` 中是否配置了 `proxy_pass`，未配置则返回 `RPS_DECLINED`
2. 填充超时和标志位默认值
3. 连接到后端 upstream（阻塞 socket + gethostbyname DNS 解析）
4. 构造代理请求并发送到后端（请求行 + Host + 自定义 header + 客户端 header + Connection: close）
5. 从后端读取响应并直接转发到客户端 socket
6. 后端连接失败或错误时返回 502 Bad Gateway

### 文件变更

| 文件 | 变更 |
|------|------|
| `src/http/modules/rps_http_proxy_module.h` | 完整重写：定义 `rps_http_proxy_header_t`、`rps_http_proxy_loc_conf_t`、模块 extern 声明 |
| `src/http/modules/rps_http_proxy_module.c` | 完整重写：8 个指令、配置生命周期、代理 handler、辅助函数，共约 580 行 |
| `src/core/rps_module.h` | 新增 `extern rps_module_t rps_http_proxy_module;` |
| `src/core/rps_module.c` | `rps_modules[]` 数组中加入 `&rps_http_proxy_module` |

### 已知限制 / TODO

1. **阻塞 I/O**：当前 connect / send / recv 均为阻塞模式，epoll 完成后需改为非阻塞 + 事件驱动
2. **响应解析简化**：目前直接把后端原始字节转发给客户端，未解析后端状态行写入 `headers_out.status`
3. **请求体转发**：`proxy_pass_request_body` 标志已解析但 handler 中未实现 body 转发
4. **分块读取**：后端响应只做一次 recv，大数据响应会被截断
5. **EAGAIN 处理**：非阻塞模式下 send/recv 遇到 EAGAIN 需注册事件重试
6. **后端 keepalive**：目前每次请求新建连接 + Connection: close
7. **变量替换**：proxy_set_header 的值目前是配置文件中写死的字符串，不支持 `$host` 等变量替换
8. **upstream 连接池**：未实现，每次请求都新建 socket + DNS 解析

### 配置示例

```nginx
http {
    server {
        listen 8080;
        server_name localhost;

        location /api/ {
            proxy_pass http://127.0.0.1:3000;
            proxy_set_header Host $proxy_host;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_connect_timeout 5000;
            proxy_read_timeout 30000;
        }

        location / {
            # 未配置 proxy_pass，由默认 handler 返回 "Hello from RPS!"
        }
    }
}
```
