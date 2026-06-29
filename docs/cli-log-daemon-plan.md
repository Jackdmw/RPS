# CLI、日志、守护进程完善计划

## 一、命令行参数扩展

当前只支持 `-c <conf_file>`，需要加：

```
./RPS -c <conf>          # 指定配置文件（已有）
./RPS -s stop            # 停止运行的 daemon 进程
./RPS -t                 # 测试配置文件语法（不启动）
./RPS -p <prefix>        # 指定安装/运行前缀路径
```

### 改动

| 文件 | 改动 |
|------|------|
| `rps.c` `rps_cli_t` | 加 `stop:1, test:1` 标志位 + `rps_str_t prefix` |
| `rps.c` `parse_cmd` | 解析新参数 |
| `rps.c` `main` | 在 `rps_init_cycle` 之前处理 `-t`；`-s stop` 分支读 PID 文件发信号后退出 |

---

## 二、Daemon 启停

### 2.1 启动（已有，需完善）

当前 `rps_daemon()` 正确（fork + setsid + 重定向 fd）。但 daemon 化前应记录 PID 到 pid 文件。

### 2.2 停止 `-s stop`

流程：

```
1. 解析 -s stop
2. 打开配置文件中的 pid 文件（或默认 run_pid.conf）
3. 读 PID
4. kill(PID, SIGTERM) 发送终止信号
5. 等待进程退出（轮询 kill(pid, 0)）
6. 超时则 kill(PID, SIGKILL)
7. unlink pid 文件
8. 退出
```

### 2.3 信号响应

| 信号 | 当前 | 需要 |
|------|------|------|
| SIGTERM | 设 `rps_term = 1` | 已有，working |
| SIGINT | 设 `rps_terminate = 1` | 已有 |
| SIGQUIT | 设 `rps_quit = 1` | 已有 |
| SIGHUP | — | 平滑重启（reload 配置）— 远期 |

---

## 三、日志系统改造

### 3.1 当前问题

- `rps_log_init` 硬编码输出到 STDERR_FILENO
- daemon 化后 STDERR 重定向到 `/dev/null`，所有日志丢失
- 没有日志文件功能
- 没有日志级别控制

### 3.2 改造方案

```
rps_log_init(rps_str_t *file_path, rps_uint_t level)
```

| 参数 | 效果 |
|------|------|
| `file_path == NULL` | 输出到 stderr |
| `file_path != NULL` | 打开指定文件（追加模式），daemon 化前就该设好 |
| `level` | 全局日志级别过滤 |

### 3.3 日志文件默认

如果配置里没指定日志文件，daemon 化时默认用 `logs/error.log`（相对 prefix 路径）。

### 3.4 改动

| 文件 | 改动 |
|------|------|
| `rps_log.h` | `rps_log_init` 签名加参数 |
| `rps_log.c` | 支持打开文件 + 追加写入 |
| `rps.c` `main` | 传入日志路径，daemon 化前打开日志文件 |
| `rps_core_module.c` | 新增 `error_log <file> <level>` 指令（远期，当前先硬编码默认值） |

---

## 四、实施顺序

1. **命令行 `-t` 测试配置** — 最简单，不涉及进程管理
2. **日志文件输出** — daemon 模式必需
3. **命令行 `-s stop`** — 依赖 pid 文件

最终 `./RPS -c test.conf` 启动 daemon，`./RPS -s stop` 停止，`./RPS -t` 检查配置。
