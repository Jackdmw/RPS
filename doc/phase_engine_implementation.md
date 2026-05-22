# 阶段引擎实现记录

## 时间
2026-05-22

## 概述
实现 HTTP 阶段引擎（phase engine），是请求处理流程的核心调度机制。
参考 nginx 的 11 阶段模型，每个请求从解析完成到响应发送，按顺序穿越所有阶段。

## 修改的文件

### 1. `src/http/rps_http_phases.h` — 阶段引擎头文件
- 移除了对 `rps_http_core_module.h` 的循环依赖，改为前向声明 `rps_http_core_main_conf_t`
- 新增 HTTP handler 返回值宏: `RPS_HTTP_OK`, `RPS_HTTP_ERROR`, `RPS_HTTP_AGAIN`, `RPS_HTTP_DECLINED`, `RPS_HTTP_DONE`
- 新增 checker 函数签名 `rps_http_phase_handler_pt`（负责流程控制）
- 完善 `rps_http_phase_handler_s` 结构体（checker + handler + next 跳转索引）
- 完善 `rps_http_phase_engine_t` 结构体（展平数组 + 关键索引）
- 修复 `rps_http_register_phase_handler` 宏，正确访问 `.handlers` 成员
- 函数声明: `rps_http_run_phases(r, cmcf)`, `rps_http_init_phase_engine(cmcf)`

### 2. `src/http/rps_http_phases.c` — 阶段引擎实现（新建）
实现了 7 个 checker 函数，每个对应不同的流程控制策略：

| checker | 适用 phase | 策略 |
|---|---|---|
| generic | POST_READ, PREACCESS, PRECONTENT, LOG | DECLINED→下一个handler; OK→下一phase; AGAIN→挂起 |
| rewrite | SERVER_REWRITE, REWRITE | DECLINED→下一个; OK→跳回FIND_CONFIG重新匹配 |
| find_config | FIND_CONFIG | 自己匹配server/location，不调任何handler |
| post_rewrite | POST_REWRITE | 检查uri_changed，是则跳回FIND_CONFIG，否则继续 |
| access | ACCESS | DECLINED→下一个; OK→下一个(鉴权通过); 拒绝→跳过后续handler |
| post_access | POST_ACCESS | 汇总access结果，继续下一phase |
| content | CONTENT | DECLINED→下一个; OK→finalize; 只执行一个handler |

实现了两个引擎函数：
- `rps_http_init_phase_engine(cmcf)`: 遍历各phase的handlers数组，展平成线性数组，
  为每个handler分配checker并计算next跳转索引。追加哨兵节点标记结束。
- `rps_http_run_phases(r, cmcf)`: 循环调checker，checker返回OK表示请求结束，
  返回AGAIN表示继续循环（phase_index已更新）。

### 3. `src/http/rps_http_core.h` — HTTP 核心头文件
- `rps_http_request_t` 新增字段:
  - `void **main_conf` — http{} 级配置指针数组
  - `void **srv_conf` — server{} 级配置指针数组
  - `unsigned uri_changed:1` — rewrite阶段是否修改了URI
- 更新 `phase_index` 注释（从"阶段内handler下标"改为"phase_engine handlers[]下标"）
- 新增 `rps_http_finalize_request(r, rc)` 函数声明

### 4. `src/http/rps_http_parse.c` — 请求创建
- `rps_http_create_request` 中初始化新增字段:
  - `main_conf = NULL`, `srv_conf = NULL`, `uri_changed = 0`

### 5. `src/http/rps_http_response.c` — HTTP 响应发送（新建）
- `rps_http_send_header(r)`: 构造HTTP响应行+基本头部，调output_filter发送
- `rps_http_send_body(r, body)`: 将body buf包装成chain节点发送
- `rps_http_output_filter(r, out)`: 遍历buffer链，逐段写入socket。支持EAGAIN重试
- `rps_http_finalize_request(r, rc)`: 请求最终回收，标记连接关闭，调close_request

### 6. `src/http/rps_http_module.c` — HTTP模块入口
- `rps_set_http_block` 在配置解析完成后，新增遍历所有HTTP模块调用postconfiguration钩子的逻辑

### 7. `src/http/modules/rps_http_core_module.h` — 核心模块配置
- `rps_http_core_main_conf_t` 补充结构体标签 `rps_http_core_main_conf_s`
- 修正 `servers` 数组成员类型注释

### 8. `src/http/modules/rps_http_core_module.c` — 核心模块实现
- 新增 `rps_http_core_postconfiguration(cf)`:
  - 注册默认content handler（"Hello from RPS!" 兜底）
  - 调用 `rps_http_init_phase_engine(cmcf)` 初始化阶段引擎
- 新增 `rps_http_core_default_handler(r)`: 默认content handler，返回 "Hello from RPS!\n"
- 模块ctx中postconfiguration钩子从NULL改为rps_http_core_postconfiguration
- 修复 `rps_http_core_create_main_conf` 中 `rps_array_init` 调用，正确传递 `&hcmcf->phases[i].handlers`

## 架构说明

### 数据流
```
请求到达 → 解析请求行/头 → rps_http_run_phases(cmcf)
                                │
                                ▼
                    while (ph[idx].checker) {
                        checker(r, ph) {
                            rc = handler(r)  ← 模块业务逻辑
                            // 根据rc更新 r->phase_index
                        }
                    }
                                │
                                ▼
                    rps_http_finalize_request(r, rc)
                    → rps_http_close_request(r)
                    → connection->close = 1
```

### handler 返回值约定
- `RPS_OK (0)`: handler处理成功 → checker决定跳转
- `RPS_DECLINED (-3)`: handler不处理 → 同phase下一个handler
- `RPS_AGAIN (-4)`: IO未就绪 → 挂起请求，等待事件唤醒后重入
- `RPS_ERROR (-1)`: 错误 → finalize
- `RPS_HTTP_DONE (-5)`: 请求处理完毕 → finalize

### checker 返回值约定
- `RPS_AGAIN`: 继续run_phases循环（phase_index已被checker更新）
- `RPS_OK`: 退出run_phases（请求已finalize或挂起）

## 待完成
- [ ] 事件循环接入（rps_worker_process_cycle 中连接 epoll + http 请求处理）
- [ ] proxy_pass 模块实现（连接后端、转发请求、读响应）
- [ ] keepalive 支持（rps_http_finalize_request 中重置请求而非关闭连接）
- [ ] 完整的 server_name / location 匹配逻辑（目前是简单前缀匹配）
- [ ] rewrite 模块（URL 改写）
- [ ] access 模块（IP 白名单/密码鉴权）
- [ ] log 模块（访问日志记录）
- [ ] listening socket 创建（rps_http_core_postconfiguration 中根据配置创建监听端口）
