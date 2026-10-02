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

## 性能测试

环境：WSL2（12 核 / 8G 内存），8 工作线程/实例，WARN 日志级别，wrk 4.1 + 自研压测客户端。

### 一、吞吐测试（wrk，keep-alive GET /api/hello）

| 场景 | 吞吐 | 平均延迟 |
|---|---|---|
| 单实例直连 8080（c=300） | 28,790 req/s | 10 ms |
| 单实例直连 8080（c=1000） | **36,843 req/s** | 27 ms |
| 3 实例经 nginx 负载均衡（c=1000） | 32,285 req/s | 30 ms |
| 短连接（每请求新建连接，c=200） | 14,446 req/s | 14 ms |

### 二、连接容量测试（自研 epoll 客户端）

> wrk 无法绑定源 IP，受"单个源 IP 可用端口约 6.4 万"限制，测不到更高连接数。
> 为此实现了独立压测客户端 `bench_many_conn.c`（多源 IP + 非阻塞 connect + HTTP
> keep-alive + epoll 管理数十万连接），以此来测量服务端的真实连接容量。

| 目标连接数 | 建连成功 | 建连失败 | 吞吐 | 服务器进程内存 |
|---|---|---|---|---|
| 2,000 | 2,000 | 0 | 41,080 req/s | — |
| 5,000 | 5,000 | 0 | 41,080 req/s | — |
| 10,000 | 10,000 | 0 | 22,584 req/s | 608 MB |
| 20,000 | 20,000 | 0 | 34,035 req/s | 128 MB |
| 40,000 | 40,000 | 0 | 31,344 req/s | 247 MB |
| 60,000 | 60,000 | 0 | 9,774 req/s | 365 MB |
| 100,000 | 100,000 | 0 | 22,584 req/s | 608 MB |
| **200,000** | **169,392** | 30,608 | 20,895 req/s | **1,021 MB** |

关键结论：

- 单实例实测支撑 **16.9 万并发连接**，每连接约 **6KB 内存**（协程帧 + 事件上下文 + Socket）；
- 服务器资源并非瓶颈：fd 用 17 万 / 上限 30 万、CPU 仅占用约 1.3 核、系统内存剩余 5GB；
- 建连耗时随连接数急剧上升：8 万 3 秒 → 10 万 80 秒 → 14 万 305 秒 → 17 万 884 秒。

### 三、内核参数调优（连接容量的前提）

| 参数 | 默认值 | 调优值 | 作用 |
|---|---|---|---|
| `ulimit -n` | 10,240 | 300,000 | 进程 fd 上限 |
| `net.ipv4.ip_local_port_range` | 32768-60999（约 2.8 万） | 1024-65535（每个源 IP 约 6.4 万） | 客户端源端口范围 |
| `net.core.somaxconn` | 4,096 | 65,535 | accept 队列长度 |
| `net.ipv4.tcp_max_syn_backlog` | 512 | 8,192 | SYN 队列长度 |
| `net.ipv4.tcp_rmem` / `tcp_wmem` | 自动调优 | 上限 64KB | 限制每连接内核 socket 缓冲（高连接数时的内存大头） |

> 注：未调优时压测 2 万连接即大量失败——定位到是"单源 IP 端口范围仅 2.8 万"所致，
> 调大端口范围后 4 万连接即通过。

### 四、瓶颈分析

连接数增长时吞吐下降、建连速率断崖式下跌，根因是**单 epoll 事件循环**：

- 为修复并发双重 resume，epoll_wait 与事件分发被串行化（`m_epollMutex`），
  同一时刻只有一个线程在等待/分发事件；
- 连接数越多，每次 `epoll_wait` 返回的事件批次越大、每连接创建协程的开销累积，
  accept 与事件分发成为瓶颈；
- 证据：CPU 仅用 1.3/12 核、内存与 fd 均有大量余量，说明瓶颈在"分发效率"而非资源；
- 演进方向：`SO_REUSEPORT` 多监听 fd + 每线程独立 epoll（one loop per thread）+ 协程线程亲和调度。

### 五、其他优化验证

| 优化项 | 效果 |
|---|---|
| 日志级别 INFO → WARN | 吞吐提升约 30 倍（INFO 每请求约 10 条日志，写盘为主要开销） |
| 写路径免挂起（send 循环到 EAGAIN，满才挂起） | c=300 +62%、c=1000 +103% |
| 双缓冲日志改造 | INFO 级别下 +7%~17% |
| 异步日志 / RabbitMQ / Redis 开关对照 | 对吞吐影响 <1%（异步设计有效） |
| 多实例负载均衡 | 3 实例直连合计约 74k req/s，接近单实例 3 倍 |

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
