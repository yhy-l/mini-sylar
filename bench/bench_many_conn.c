#define _GNU_SOURCE
// 多源IP大量连接压测客户端（epoll + 非阻塞connect + HTTP keep-alive）
// 三阶段：1)并发发起连接 2)等全部建立 3)统一发请求压测
// 用法: ./bench_many_conn <连接数> <源IP个数> <压测秒数> [端口]
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
#define RBUF_SIZE  1024

static const char REQ[] =
    "GET /api/hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
#define REQ_LEN (sizeof(REQ) - 1)

enum { ST_CONNECTING = 0, ST_IDLE = 1, ST_SENDING = 2, ST_READING = 3 };

typedef struct {
    int    fd;
    int    state;
    size_t sent;
    char   rbuf[RBUF_SIZE];
    size_t rlen;
    int    header_end;
    long   body_need;
} Conn;

static Conn  *conns;
static size_t total_conn;
static long   done_requests;
static long   conn_ok;
static long   conn_fail;
static int    g_epfd;

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void mod_event(Conn *c, uint32_t events)
{
    struct epoll_event ev;
    ev.events = events;
    ev.data.u64 = (uint64_t)(c - conns);
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static void reset_conn_state(Conn *c)
{
    c->rlen = 0;
    c->header_end = -1;
    c->body_need = 0;
    c->sent = 0;
}

static void send_request(Conn *c)
{
    c->state = ST_SENDING;
    reset_conn_state(c);
    mod_event(c, EPOLLOUT);
}

static void mark_idle(Conn *c)
{
    c->state = ST_IDLE;
    mod_event(c, 0);   // 建连完成，暂不监听事件
}

static int parse_response(Conn *c)
{
    if (c->header_end < 0) {
        for (size_t i = 0; i + 3 < c->rlen; ++i) {
            if (c->rbuf[i] == '\r' && c->rbuf[i+1] == '\n' &&
                c->rbuf[i+2] == '\r' && c->rbuf[i+3] == '\n') {
                c->header_end = (int)(i + 4);
                c->rbuf[c->rlen] = '\0';
                char *p = strcasestr(c->rbuf, "content-length:");
                c->body_need = p ? atol(p + 15) : 0;
                break;
            }
        }
        if (c->header_end < 0) return 0;
    }
    return (long)(c->rlen - c->header_end) >= c->body_need;
}

static void try_write(Conn *c)
{
    while (c->sent < REQ_LEN) {
        ssize_t n = send(c->fd, REQ + c->sent, REQ_LEN - c->sent, MSG_NOSIGNAL);
        if (n > 0) { c->sent += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        return;
    }
    c->state = ST_READING;
    mod_event(c, EPOLLIN);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "用法: %s <连接数> <源IP个数> <压测秒数> [端口]\n", argv[0]);
        return 1;
    }
    total_conn = (size_t)atol(argv[1]);
    int src_ips  = atoi(argv[2]);
    int duration = atoi(argv[3]);
    int port     = argc > 4 ? atoi(argv[4]) : 8080;

    conns = calloc(total_conn, sizeof(Conn));
    if (!conns) { perror("calloc"); return 1; }

    g_epfd = epoll_create1(0);
    int epfd = g_epfd;
    struct epoll_event ev, events[MAX_EVENTS];

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

    // ---------- 阶段1：并发发起连接 ----------
    double t_start = now_sec();
    for (size_t i = 0; i < total_conn; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) { ++conn_fail; continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        struct sockaddr_in src;
        memset(&src, 0, sizeof(src));
        src.sin_family = AF_INET;
        char ip[32];
        snprintf(ip, sizeof(ip), "127.0.0.%d", (int)(i % (size_t)src_ips) + 1);
        inet_pton(AF_INET, ip, &src.sin_addr);
        if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) {
            close(fd); ++conn_fail; continue;
        }
        if (connect(fd, (struct sockaddr *)&srv, sizeof(srv)) < 0 && errno != EINPROGRESS) {
            close(fd); ++conn_fail; continue;
        }
        conns[i].fd = fd;
        conns[i].state = ST_CONNECTING;
        conns[i].header_end = -1;

        ev.events = EPOLLOUT;
        ev.data.u64 = i;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

        if ((i + 1) % 10000 == 0) {
            int n = epoll_wait(epfd, events, MAX_EVENTS, 0);
            for (int k = 0; k < n; ++k) {
                size_t idx = events[k].data.u64;
                Conn *c = &conns[idx];
                if (c->fd < 0 || c->state != ST_CONNECTING) continue;
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) { ++conn_ok; mark_idle(c); }
                else { epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL); ++conn_fail; close(c->fd); c->fd = -1; }
            }
            fprintf(stderr, "  已发起 %zu 个连接 (成功%ld 失败%ld, 用时%.0fs)\n",
                    i + 1, conn_ok, conn_fail, now_sec() - t_start);
        }
    }

    // ---------- 阶段2：等所有连接建立完毕 ----------
    fprintf(stderr, "等待剩余连接建立完成...\n");
    double t_wait = now_sec();
    double last_rep = 0;
    while ((size_t)(conn_ok + conn_fail) < total_conn) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 500);
        for (int k = 0; k < n; ++k) {
            size_t idx = events[k].data.u64;
            Conn *c = &conns[idx];
            if (c->fd < 0 || c->state != ST_CONNECTING) continue;
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                ++conn_fail; close(c->fd); c->fd = -1;
            } else { ++conn_ok; mark_idle(c); }
        }
        double e = now_sec() - t_wait;
        if (e - last_rep >= 3.0) {
            fprintf(stderr, "  [等待%.0fs] 已建立=%ld 失败=%ld\n", e, conn_ok, conn_fail);
            last_rep = e;
        }
        if (e > 240.0) { fprintf(stderr, "  建连等待超时\n"); break; }
    }
    fprintf(stderr, "建连完成：成功=%ld 失败=%ld，总耗时 %.1fs\n",
            conn_ok, conn_fail, now_sec() - t_start);

    // ---------- 阶段3：统一发请求压测 ----------
    long active = 0;
    for (size_t i = 0; i < total_conn; ++i) {
        if (conns[i].fd >= 0 && conns[i].state == ST_IDLE) {
            send_request(&conns[i]);
            ++active;
        }
    }
    fprintf(stderr, "参与压测的连接数: %ld\n", active);

    double t_bench = now_sec();
    double last_report = 0;
    long last_requests = 0;
    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 200);
        for (int k = 0; k < n; ++k) {
            size_t idx = events[k].data.u64;
            Conn *c = &conns[idx];
            if (c->fd < 0) continue;
            uint32_t rev = events[k].events;

            if (c->state == ST_SENDING && (rev & EPOLLOUT)) {
                try_write(c);
            }
            if ((rev & (EPOLLIN | EPOLLHUP | EPOLLERR)) && c->state == ST_READING) {
                ssize_t r = recv(c->fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen, 0);
                if (r > 0) {
                    c->rlen += (size_t)r;
                    if (parse_response(c)) {
                        ++done_requests;
                        send_request(c);
                    } else if (c->rlen >= RBUF_SIZE) {
                        send_request(c);
                    }
                } else if (r == 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd); c->fd = -1; ++conn_fail;
                }
            }
        }
        double elapsed = now_sec() - t_bench;
        if (elapsed - last_report >= 1.0) {
            double qps = (done_requests - last_requests) / (elapsed - last_report);
            fprintf(stderr, "  [压测%.0fs] 连接=%ld 完成请求=%ld 当前QPS=%.0f\n",
                    elapsed, conn_ok, done_requests, qps);
            last_report = elapsed;
            last_requests = done_requests;
        }
        if (elapsed >= duration) break;
    }
    double secs = now_sec() - t_bench;

    printf("\n===== 结果 =====\n");
    printf("目标连接数   : %zu\n", total_conn);
    printf("建连成功     : %ld\n", conn_ok);
    printf("建连失败     : %ld\n", conn_fail);
    printf("压测时长     : %.2fs\n", secs);
    printf("完成请求数   : %ld\n", done_requests);
    printf("平均 QPS     : %.0f\n", done_requests / secs);

    for (size_t i = 0; i < total_conn; ++i)
        if (conns[i].fd >= 0) close(conns[i].fd);
    close(epfd);
    free(conns);
    return 0;
}
