# 压测工具

同机压测时，**压测端自身的 CPU 开销会决定你测到的上限**。这两个客户端就是为了
解决不同层次的问题而写的。

## 1. `bench_many_conn.c` —— 高并发连接容量测试

`wrk` 无法绑定源 IP，受"单个源 IP 可用端口约 6.4 万"的限制，测不出更高的连接数。
这个客户端用**多源 IP + 非阻塞 connect + epoll** 管理数十万连接，
用来测量服务器的真实连接容量。

```bash
gcc -O2 -o bench_many_conn bench_many_conn.c
./bench_many_conn <连接数> <源IP个数> <压测秒数> [端口]

# 例：40 万连接、64 个源 IP、压 10 秒
./bench_many_conn 400000 64 10 8080
```

配合以下内核参数（临时生效，重启失效）：

```bash
sudo sysctl -w fs.file-max=4194304
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sudo sysctl -w net.ipv4.ip_local_port_range="10000 65535"
ulimit -n 1048576
```

## 2. `fast_client.c` —— 服务器吞吐上限测试

用 `wrk` 压测时，wrk 自己每请求也要约 14 µs CPU，和被测服务器是一个量级，
同机测试会**两边同时饱和**，测到的是机器的总吞吐而不是服务器上限
（表现：wrk 线程数从 4 加到 12，吞吐跟着涨，而服务器 CPU 几乎不动）。

这个客户端把压测端做"便宜"：

- **不做 HTTP 解析**：响应是定长的，直接按字节计数
- **支持流水线**：一次 `send` 发出 `depth` 个请求，再批量收响应，
  每请求系统调用降到约 0.1 次

于是瓶颈真正转移到服务器侧。

```bash
gcc -O2 -o fast_client fast_client.c

# 常规（每连接一次请求-响应）
./fast_client 5000 8 10 8080 1

# 流水线深度 64：测服务器请求处理能力上限
./fast_client 5000 8 10 8080 64

# 流水线深度 0：只建立并保持连接、不发请求（测纯连接容量，需外部 kill 结束）
./fast_client 37500 64 1 8080 0
```

参数：`<连接数> <源IP数> <压测秒数> [端口] [流水线深度]`

## 测量口径（重要）

| 口径 | 工具 | 说明 |
|---|---|---|
| 常规长连接吞吐 | `wrk -t8 -c512~1024` | 接近真实客户端行为 |
| 服务器处理能力上限 | `fast_client` 流水线 | 压测端开销被压到最低后的服务器上限 |
| 连接容量 | `bench_many_conn` / `fast_client depth=0` | 只建立并保持连接 |

**报数字时必须带口径**——不同口径测的是不同的东西，混着比会得出错误结论。
