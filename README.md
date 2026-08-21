# mini-sylar 高性能服务器

基于 C++23 协程 + epoll 的高性能 HTTP/TCP 服务器，仿 sylar 架构实现。

单进程多线程、协程化事件驱动：所有协程任务由调度器统一管理，IO 统一交给 epoll，
协程遇到 IO 就挂起、事件就绪再恢复——用顺序代码实现高并发，避免回调地狱。

## 技术栈

- C++23 协程（std::coroutine）、epoll（边缘触发）、非阻塞 socket
- 8 工作线程协程调度器（Scheduler / IOManager）
- RabbitMQ：异步日志管道（生产者发 → 消费者按级别分文件、告警、统计）
- Redis：接口限流（INCR + EXPIRE → 429）+ 实时统计（/api/stats）
- nginx：静态文件直出 + API 反向代理 + 多实例负载均衡
- CMake / g++-14，Linux / WSL 环境

## 架构

```
                     ┌─ 静态文件(html/css/js) → nginx 直接返回
用户 ── 80端口 nginx ─┤
                     └─ /api/* → 反向代理 ──→ mini-sylar (8080/8082/8083)
                                                    │
                     ┌──────────────────────────────┤
                     ▼                              ▼
              RabbitMQ(日志管道)              Redis(限流+统计)
                     │                              │
              log_consumer                      /api/stats
             (分文件/告警/统计)
```

## 核心设计

- **协程调度**：Task（promise_type 协议）→ 调度队列 → 工作线程 resume；
  协程挂起（WAITING_IO）后不占线程，等 epoll 事件由 IOManager 唤醒。
- **异步日志**：业务线程只把日志入队（有界队列，满时丢弃+计数），
  后台线程批量格式化、一次写盘 + 发 RabbitMQ，业务永不被日志拖垮。
- **MQ 日志管道**：日志 JSON 化发到 RabbitMQ，独立消费者进程按级别
  写 info.log / error.log / structured.log，60 秒内错误 ≥10 条告警，输出统计。
- **Redis 限流**：专职 Redis 线程 + 有界队列，业务协程 co_await 挂起等结果；
  Redis 挂掉自动降级放行，服务不中断。
- **多实例扩展**：端口参数化（`./mini-sylar [http端口] [tcp端口]`），
  nginx upstream 轮询多个后端；Redis/MQ 天然共享，实例无状态可水平扩展。

## 快速开始

### 依赖

```bash
sudo apt install g++-14 cmake redis-server libhiredis-dev \
                 rabbitmq-server librabbitmq-dev nlohmann-json3-dev nginx
```

### 构建

```bash
cd build
cmake .. -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_CXX_FLAGS="-std=c++23"
cmake --build . -j 4
```

### 运行

```bash
sudo service redis-server start       # Redis：限流 + 统计
sudo service rabbitmq-server start    # RabbitMQ：日志管道
./log_consumer                        # MQ 消费者：写 build/logs/ 下各类日志
./mini-sylar                          # 服务器（默认 8080；多实例指定不同端口）
```

### 接口

| 接口 | 说明 |
|---|---|
| `GET /` | 首页（静态文件） |
| `GET /api/hello` / `GET /api/time` | JSON 接口 |
| `POST /api/echo` | 回显接口 |
| `GET /api/limited` | 限流演示：每 IP 每 60 秒最多 10 次，超限 429 |
| `GET /api/stats` | Redis 实时统计（total / conns / status / api） |
| `GET /text` | 纯文本接口 |

## 压测数据（WSL 环境，8 线程/实例，WARN 日志）

| 场景 | 吞吐 |
|---|---|
| 单实例直连 8080（wrk c=1000） | ~37k req/s |
| 3 实例经 nginx 80 端口 | ~32k req/s |
| 3 实例直连并行 | ~74k req/s |
| 8000 并发连接 | 稳定，吞吐不降 |

压测还验证了：异步日志/MQ/Redis 对吞吐几乎零影响；写路径免挂起优化
（send 循环到 EAGAIN）带来 +62%~103% 提升。

## 项目结构

```
mini-sylar/
├── main.cpp               # 入口：HTTP(8080) + TCP(8081)，端口可参数化
├── task.h                 # 协程任务（promise_type / 句柄生命周期）
├── scheduler.*            # 协程调度器（任务队列 + 工作线程）
├── iomanager.*            # epoll 事件循环（fd → IOContext → 唤醒协程）
├── socket.* / address.*   # Socket 与地址封装
├── tcpserver.*            # TCP/HTTP 服务器（监听/accept/客户端集合）
├── http/                  # HTTP 解析、请求/响应、HttpServer
├── log.*                  # 日志系统（Logger/Appender/Formatter）
├── async_log_appender.h   # 异步日志（有界队列 + 后台线程）
├── mq_log_sender.*        # RabbitMQ 日志生产者
├── log_consumer.cpp       # MQ 日志消费者（分文件/告警/统计）
├── redis_client.*         # Redis 异步客户端（限流 + 统计）
├── web/                   # 前端页面
├── nginx.conf             # nginx 动静分离 / 负载均衡配置
└── regression.sh          # 功能回归脚本
```

## 注意事项

- `nginx.conf` 中的 `root` 是绝对路径（nginx 要求），部署时按实际
  `web/` 目录位置修改，并复制到 `/etc/nginx/sites-available/`。
- 日志文件路径相对启动目录，建议从 `build/` 启动。
- RabbitMQ / Redis 未启动时自动降级：日志只写本地、限流放行，服务不中断。

## License

MIT
