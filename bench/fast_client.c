#define _GNU_SOURCE
// 极简 HTTP 压测客户端：只做 连接/发送/接收/字节计数，不做任何HTTP解析。
//
// 为什么需要它：wrk 这类通用客户端每个请求要花十几微秒 CPU（HTTP状态机+解析+
// 每请求3次系统调用），和被测服务器差不多，同机上压测永远是"两边同时饱和"，
// 测出来的是机器的总吞吐，不是服务器的上限。
//
// 这个客户端支持"流水线"：一次 send 发出 depth 个请求，再批量收响应。
// depth=16 时平均每请求只要 0.1 次系统调用，压测端CPU降到 1~2µs/请求，
// 服务器才能真正变成瓶颈。
//
// 用法: fast_client <连接数> <源IP数> <压测秒数> [端口] [流水线深度]
//       流水线深度 = 0  → 只建立并保持连接、不发请求（测连接容量用）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MAX_EVENTS 8192

static const char REQ[] =
    "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
#define REQ_LEN (sizeof(REQ) - 1)

enum { ST_CONNECTING = 0, ST_NEED_SEND = 1, ST_WAIT_RESP = 2 };

typedef struct {
    int    fd;
    int    state;
    size_t sent_off;    // 本批次已经发出去多少字节
    int    resp_todo;   // 本批次还差几个响应
    long long acc;      // 已收到、还没凑够一个完整响应的字节数
} Conn;

static Conn  *conns;
static size_t total_conn;
static long long done_requests;
static long long resp_size;
static int    epfd;
static int    g_depth = 1;
static char  *g_batch;        // 预拼好的 depth 个请求
static size_t g_batch_len;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void mod_event(Conn *c, uint32_t ev)
{
    struct epoll_event e;
    e.events = ev;
    e.data.u64 = (uint64_t)(c - conns);
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &e);
}

/// 预跑一次量出响应长度（响应定长，之后只靠字节数计数，不做解析）
static long long probe_resp_size(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);
    if (connect(fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) { close(fd); return -1; }
    if (send(fd, REQ, REQ_LEN, 0) != (ssize_t)REQ_LEN) { close(fd); return -1; }

    char buf[8192];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    close(fd);
    if (n <= 0) return -1;

    long long head = -1;
    for (ssize_t i = 0; i + 3 < n; ++i) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') { head = i + 4; break; }
    }
    if (head < 0) return -1;
    long long clen = 0;
    for (ssize_t i = 0; i + 16 < n; ++i) {
        if (strncasecmp(buf + i, "content-length:", 15) == 0) { clen = atoll(buf + i + 15); break; }
    }
    return head + clen;
}

/// 把本批次的请求尽量发出去；发完即转入等响应。返回1=发完，0=等可写，-1=出错
static int try_send_batch(Conn *c)
{
    while (c->sent_off < g_batch_len) {
        ssize_t w = send(c->fd, g_batch + c->sent_off, g_batch_len - c->sent_off, MSG_NOSIGNAL);
        if (w > 0) { c->sent_off += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            mod_event(c, EPOLLOUT);
            return 0;
        }
        return -1;
    }
    c->sent_off = 0;
    c->resp_todo = g_depth;
    c->acc = 0;
    c->state = ST_WAIT_RESP;
    mod_event(c, EPOLLIN);
    return 1;
}

int main(int argc, char **argv)
{
    total_conn = (argc > 1) ? strtoul(argv[1], NULL, 10) : 1000;
    int src_ips = (argc > 2) ? atoi(argv[2]) : 8;
    int secs    = (argc > 3) ? atoi(argv[3]) : 8;
    int port    = (argc > 4) ? atoi(argv[4]) : 8080;
    g_depth     = (argc > 5) ? atoi(argv[5]) : 1;
    int hold_mode = (g_depth <= 0);      // 0 = 只挂连接不请求
    if (g_depth < 1) g_depth = 1;

    resp_size = probe_resp_size(port);
    if (resp_size <= 0) { fprintf(stderr, "探测响应长度失败\n"); return 1; }

    g_batch_len = (size_t)g_depth * REQ_LEN;
    g_batch = malloc(g_batch_len);
    for (int i = 0; i < g_depth; ++i) memcpy(g_batch + (size_t)i * REQ_LEN, REQ, REQ_LEN);
    fprintf(stderr, "响应长度=%lld字节 流水线深度=%d 单批发送=%zu字节%s\n",
            resp_size, g_depth, g_batch_len, hold_mode ? "  [只持连接模式]" : "");

    conns = calloc(total_conn, sizeof(Conn));
    if (!conns) { perror("calloc"); return 1; }
    epfd = epoll_create1(0);

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

    struct epoll_event ev, events[MAX_EVENTS];

    // ---- 阶段1：并发建连 ----
    double t0 = now_sec();
    long fail = 0;
    for (size_t i = 0; i < total_conn; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) { ++fail; continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        struct sockaddr_in src;
        memset(&src, 0, sizeof(src));
        src.sin_family = AF_INET;
        char ip[32];
        snprintf(ip, sizeof(ip), "127.0.0.%d", (int)(i % (size_t)src_ips) + 1);
        inet_pton(AF_INET, ip, &src.sin_addr);
        if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) { close(fd); ++fail; continue; }
        if (connect(fd, (struct sockaddr *)&srv, sizeof(srv)) < 0 && errno != EINPROGRESS) {
            close(fd); ++fail; continue;
        }
        conns[i].fd = fd;
        conns[i].state = ST_CONNECTING;
        conns[i].sent_off = 0;
        conns[i].resp_todo = 0;
        conns[i].acc = 0;
        ev.events = EPOLLOUT;
        ev.data.u64 = i;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
    fprintf(stderr, "建连发起完毕 用时%.1fs 失败%ld\n", now_sec() - t0, fail);

    // ---- 阶段2：等全部连接建立，连上就发第一批 ----
    long long ok = 0;
    while ((size_t)(ok + fail) < total_conn) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 200);
        if (n <= 0) continue;
        for (int k = 0; k < n; ++k) {
            size_t idx = events[k].data.u64;
            Conn *c = &conns[idx];
            if (c->fd < 0 || c->state != ST_CONNECTING) continue;
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) { ++ok; c->state = ST_NEED_SEND; try_send_batch(c); }
            else { epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL); close(c->fd); c->fd = -1; ++fail; }
        }
    }
    fprintf(stderr, "建连完成 %lld/%zu 用时%.1fs\n", ok, total_conn, now_sec() - t0);

    // ---- 阶段3：压测 ----
    if (hold_mode) {
        // 只保持连接不断开：连接全部建立后原地挂着，等被外部 kill
        fprintf(stderr, "连接已全部建立，进入保持模式（不下发请求）\n");
        for (;;) sleep(3600);
    }

    double t_start = now_sec(), t_last = t_start, last_req = 0;
    char buf[8192];

    while (now_sec() - t_start < secs) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 200);
        if (n <= 0) continue;
        for (int k = 0; k < n; ++k) {
            size_t idx = events[k].data.u64;
            Conn *c = &conns[idx];
            if (c->fd < 0) continue;

            if (events[k].events & (EPOLLHUP | EPOLLERR)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                close(c->fd); c->fd = -1; continue;
            }

            if (c->state == ST_NEED_SEND) {
                if (try_send_batch(c) < 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd); c->fd = -1;
                }
                continue;
            }

            if (c->state == ST_WAIT_RESP) {
                if (!(events[k].events & EPOLLIN)) continue;
                for (;;) {
                    ssize_t r = recv(c->fd, buf, sizeof(buf), 0);
                    if (r > 0) {
                        c->acc += r;
                        // 响应定长，直接按字节数计数，不做解析
                        while (c->acc >= resp_size) {
                            c->acc -= resp_size;
                            ++done_requests;
                            if (--c->resp_todo <= 0) {
                                c->state = ST_NEED_SEND;
                                if (try_send_batch(c) < 0) { c->fd = -1; }
                                break;
                            }
                        }
                        if (c->fd < 0) break;
                        // 本批还没收完就继续读；本批已收完则跳出，等下一批
                        if (c->state != ST_WAIT_RESP) break;
                    } else if (r == 0) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                        close(c->fd); c->fd = -1;
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                            close(c->fd); c->fd = -1;
                        }
                        break;
                    }
                }
            }
        }

        double now = now_sec();
        if (now - t_last >= 1.0) {
            fprintf(stderr, "  [%2.0fs] 请求=%lld QPS=%.0f\n",
                    now - t_start, done_requests,
                    (double)(done_requests - last_req) / (now - t_last));
            last_req = done_requests;
            t_last = now;
        }
    }

    double elapsed = now_sec() - t_start;
    printf("建连成功 : %lld\n", ok);
    printf("建连失败 : %ld\n", fail);
    printf("压测时长 : %.2fs\n", elapsed);
    printf("完成请求 : %lld\n", done_requests);
    printf("平均 QPS : %.0f\n", done_requests / elapsed);
    return 0;
}
