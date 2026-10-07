# mini-sylar — C++23 协程 + epoll 高性能 HTTP/TCP 服务器

用 **C++23 原生协程** 把异步 IO 写成顺序代码：协程遇到 IO 自动挂起、事件就绪自动恢复，
用少量线程支撑海量并发连接，避免回调地狱。

架构为 **单进程内多个事件循环实例**（每实例 1 个工作线程 + 1 个独立 epoll），
连接由「单监听 + 轮转分发」均匀投递，等价于 one loop per thread。

## 性能指标

环境：WSL2，12 核 / 8 GB 内存，日志级别 WARN，接口 `GET /health`。

| 指标 | 实测 | 测量口径 |
|---|---|---|
| **常规吞吐峰值** | **465,502 req/s** | `wrk -t8 -c1024`，keep-alive（此时服务器只占 5.68/12 核） |
| **请求处理能力上限** | **1,203,113 req/s** | 自研流水线压测端，服务器占 10.3/12 核 |
| **并发连接** | **570,996** | 16 进程 × 3.75 万，只建立并保持连接；服务端 CPU 0.00 核 |
| 单连接串行延迟 | **69 µs** | `wrk -t1 -c1` |
| 每请求服务器 CPU | 8.6 µs（流水线）～ 12.2 µs | 核数 ÷ 吞吐 |
| 空闲连接内存 | **1.9 KB / 连接** | 服务端 RSS 增量 ÷ 连接数 |

> **口径说明**：常规压测（wrk）更接近真实客户端行为，但 wrk 自身每请求也要约 14 µs CPU，
> 同机测试时会与服务器同时饱和；换成流水线压测端（压测开销降到约 1 µs/请求）后才能
> 测到服务器真正的处理上限。两组数字测的是不同的东西，均已标注口径。
> 压测工具源码见 [`bench/`](bench/)。

## 技术栈

C++23 协程（`std::coroutine`）· epoll 边缘触发 · 多实例多线程 · 非阻塞 socket ·
CMake + g++-14（-O3 + LTO）· Redis · RabbitMQ · nginx · Docker

## 架构

```
                          ┌── 静态资源 (html/css/js) ──→ nginx 直接返回
客户端 ── 80 ── nginx ────┤
                          └── /api/* ── 反向代理 ──→ mini-sylar 实例 1/2/3
                                                          │
                     ┌────────────────────────────────────┼──────────────────┐
                     ▼                                    ▼                  ▼
              RabbitMQ（日志管道）                   Redis（限流+统计）   本地异步日志
                     │                                    │
              log_consumer                              /api/stats
        （按级别分文件 / 错误告警 / 统计）

单进程内部（HTTP 服务）：
   实例 0 ───── 1 线程 + 1 epoll（负责 accept + 处理被分到的连接）
   实例 1~11 ── 各 1 线程 + 1 epoll（只处理被分到的连接）
        ↑
   accept 到连接后，pickIoWorker() 轮转选一个实例投递
```

## 核心设计

### 1. 协程调度器：把异步 IO 写成顺序代码

`Task` 定义协程的 `promise_type`（`initial_suspend` 挂起、句柄交给调度器），
`Scheduler` 维护任务队列 + 工作线程，`IOManager` 在 `Scheduler` 之上接入 epoll：

- 协程 `co_await waitReadAsync(fd)` 时把**自己的句柄**登记进 `IOContext` 并挂起，不占线程；
- epoll 事件到达后，`IOManager` 取出句柄重新入队，由工作线程 `resume`；
- 协程帧只保存局部变量和挂起点状态，因此**每连接内存只要约 500 字节**。

于是业务代码是 `recv → 解析 → 处理 → send` 的顺序写法，并发能力来自"挂起"而不是"线程数"。

### 2. 多实例 + 每实例独立 epoll（性能架构的关键）

早期是「一个 epoll + 8 线程共享」，为保证正确性加了一把 `m_epollMutex`
（避免同一协程被两个线程并发 `resume` 导致堆损坏），代价是 8 线程退化成单事件循环
（实证：线程 8→16 只提升 16%）。

现在改为 **12 个实例，每实例 1 线程 + 1 个独立 epoll**，锁变成无竞争，
等价 one loop per thread。连接分发不用 `SO_REUSEPORT`（内核按四元组哈希分发会导致
实例间负载不均），而是**单监听 + 轮转分发**。

### 3. 读事件常驻注册

长连接上读事件注册一次后不再摘除，下次 `waitRead` 直接复用，省掉每请求
`epoll_ctl(ADD/DEL)` 两次系统调用（perf 显示该项曾占 11.3% CPU）。
配套用 `waitEvents`（只唤醒真正在等该事件的协程）和 `pendingEvents` +
`await_ready()`（事件早到时记下来，下次挂起前直接返回就绪）保证不丢事件、不误唤醒。

### 4. 异步日志：双缓冲 + 后台线程 + MQ 管道

业务线程只把日志写进前端缓冲，缓冲满时与后端缓冲 `swap` 后交给后台线程批量落盘，
业务线程永不阻塞（有界队列写满时丢弃并计数）。日志同时 JSON 化发往 RabbitMQ，
由独立的 `log_consumer` 进程按级别分文件、统计错误数并告警。

### 5. Redis：接口限流与实时统计

专职 Redis 线程 + 有界队列，业务线程只入队：

- **限流**（`INCR` + `EXPIRE` 固定窗口，超限返回 429）需要结果 → 协程 `co_await` 挂起，
  Redis 线程执行完 `schedule` 回协程所属实例唤醒；
- **统计**不需要结果 → `countAsync` 入队即走，且按"线程本地聚合 + 攒够一批或满 1 秒上报"
  批量发送，把每请求 3 条命令降到约 1/1024 条；
- Redis 不可用时自动降级（统计丢弃、查询放行），不影响服务可用性。

### 6. nginx 动静分离 + 多实例负载均衡

静态资源由 nginx 直出，`/api/*` 反向代理到多个后端实例；
实例无状态（限流/统计共享 Redis、日志共享 MQ），可水平扩展。

## 性能优化过程

在同一口径（12 实例、`wrk -t8 -c512`、`/health`）下逐步优化并复测：

| 步骤 | 改动 | 吞吐 req/s | 相对上一步 |
|---|---|---|---|
| 0 | 优化前基线 | 30,767 | — |
| 1 | 调度器去掉"队列空先等 10ms"的固定等待 | 142,048 | +362% |
| 2 | 缓存热点路径上的日志器（避免每次抢全局锁） | 174,414 | +23% |
| 3 | 同线程投递不再写唤醒管道 | 184,061 | +5.5% |
| 4 | Redis 统计改本地聚合 + 批量上报 | 216,244 | +17.5% |
| 5 | 编译默认 Release（-O3，此前等价 -O0） | 354,935 | +64% |
| 6 | 读事件常驻注册 | 393,377 | +10.8% |
| 7 | HTTP 头部容器由哈希表改为小数组 | 396,604 | +0.8% |
| 8 | 开启 LTO | **413,137** | +4.2% |

其中收益最大的两项都不是算法问题，而是"去掉了本来就不该做的事"：

- **编译没开优化**：`CMAKE_BUILD_TYPE` 为空导致等价 `-O0`，perf 显示 54% 的 CPU
  耗在字符串操作上（本该被内联的一行访问器全成了真实函数调用）；
- **调度器每次处理完一批请求要白等 10ms**：队列空时先 `wait_for(10ms)` 才去
  `epoll_wait`，单连接串行延迟因此固定为 10.25 ms。

## 快速开始

### 依赖

```bash
sudo apt install g++-14 cmake make \
                 libhiredis-dev redis-server \
                 librabbitmq-dev rabbitmq-server nlohmann-json3-dev nginx
```

### 构建与运行

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_CXX_FLAGS="-std=c++23"
cmake --build . -j 4

# 启动中间件（可选，未启动时自动降级）
sudo service redis-server start
sudo service rabbitmq-server start

./log_consumer &                 # 可选：MQ 日志消费者
./mini-sylar                     # HTTP 8080 + TCP 8081
./mini-sylar 8082 8083           # 多实例：指定不同端口
```

环境变量：`REDIS_HOST` / `MQ_HOST` / `HTTP_INSTANCES` / `HTTP_STATS`。

### Docker 一键部署

```bash
docker compose up -d --build
# 浏览器打开 http://localhost
```

`docker compose` 会启动 3 个后端实例 + Redis + RabbitMQ + 日志消费者 + nginx，
无需手动安装任何中间件。

### 功能回归

```bash
bash regression.sh    # 15 项：接口 / 静态文件 / nginx / 限流 / 统计 / TCP
```

## 接口

| 接口 | 说明 |
|---|---|
| `GET /` · `/about.html` · `/css/*` · `/js/*` | 静态资源 |
| `GET /health` | 健康检查（返回固定短文本，用于压测） |
| `GET /api/hello` · `GET /api/time` | JSON 接口 |
| `POST /api/echo` | 回显请求体 |
| `GET /api/limited` | 限流演示：每 IP 每 60 秒最多 10 次，超限返回 429 |
| `GET /api/stats` | Redis 实时统计（total / conns / status / api） |
| `GET /text` | 纯文本接口 |

## 目录结构

```
mini-sylar/
├── main.cpp                  入口：创建 12 个 IO 实例 + TCP 实例，处理信号
├── task.h                    协程任务（promise_type / 句柄生命周期）
├── scheduler.{h,cpp}         协程调度器（任务队列 + 工作线程 + 停止流程）
├── iomanager.{h,cpp}         epoll 事件循环（IOContext / 常驻注册 / 唤醒管道）
├── socket.{h,cpp}            Socket 封装（非阻塞 / cancelAll / 超时）
├── address.{h,cpp}           地址与网卡信息
├── tcpserver.{h,cpp}         监听 / accept 排空 / 活动连接集合 / 优雅关停
├── http/                     HTTP 解析、请求响应、HttpServer（路由 + 静态文件）
├── log.{h,cpp}               日志系统（Logger / Appender / Formatter）
├── async_log_appender.h      异步日志（双缓冲 + 后台批量落盘）
├── mq_log_sender.{h,cpp}     RabbitMQ 日志生产者
├── log_consumer.cpp          MQ 日志消费者（分级别文件 / 告警 / 统计）
├── redis_client.{h,cpp}      Redis 异步客户端（限流 + 统计）
├── web/                      前端页面（由 nginx 或服务端静态目录提供）
├── bench/                    压测工具（高并发连接 / 流水线吞吐）见 bench/README.md
├── nginx.conf                nginx 动静分离 + 上游负载均衡
├── Dockerfile                多阶段构建
├── docker-compose.yml        一键拉起全栈（3 实例 + Redis + MQ + nginx）
└── regression.sh             功能回归脚本（15 项）
```

## 设计取舍与后续方向

- **epoll 串行化**：`m_epollMutex` 保证同一协程不会被并发 resume（正确性优先），
  当前用"每实例单线程 + 多实例"绕开它；彻底方案是每线程独立 epoll + 协程线程亲和。
- **连接分发**：单监听 + 轮转保证均匀，但 accept 集中在实例 0，连接风暴下会成为单点。
- **协议**：目前只实现 HTTP/1.1（含 Content-Length 定界与 keep-alive），
  未实现 chunked、HTTP/2、TLS 与 IPv6。
- **静态文件**：每次读盘，未做零拷贝（sendfile）与内存缓存。
- **连接超时**：读等待的超时参数已预留但尚未接入定时器，空闲连接靠对端断开回收。
- 当前性能数据均在 **WSL2** 环境测得（回环走虚拟网卡），真机 Linux 的绝对值会更好。

## License

MIT
