// src/net/tcp_server.cpp
#include "tcpserver.h"
#include <sys/socket.h>
#include <memory>
#include <vector>
#include "log.h"
#include <functional>
#include <cstdio>
#include <algorithm>

namespace sylar {

    static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

    namespace {

        /**
         * 把二进制数据转成可读形式用于日志
         * 可打印ASCII原样输出，其余字节转义为 \xHH；
         * 过长数据只预览前MAX_SHOW个字节，避免日志行爆炸
         */
        std::string to_printable(const char* data, size_t len)
        {
            const size_t MAX_SHOW = 512;
            size_t show = std::min(len, MAX_SHOW);

            std::string out;
            out.reserve(show * 4 + 8);
            for (size_t i = 0; i < show; ++i) {
                unsigned char c = static_cast<unsigned char>(data[i]);
                if (c >= 0x20 && c <= 0x7e) {
                    out += static_cast<char>(c);
                } else {
                    char hex[5];
                    snprintf(hex, sizeof(hex), "\\x%02x", c);
                    out += hex;
                }
            }
            if (len > MAX_SHOW) {
                out += "...(共" + std::to_string(len) + "字节)";
            }
            return out;
        }

    } // namespace


    TcpServer::TcpServer(sylar::IOManager* io_worker,
                         sylar::IOManager* accept_worker)
        :m_ioWorker(io_worker)
        ,m_acceptWorker(accept_worker)
        ,m_recvTimeout(0)
        ,m_name("sylar/1.0.0")
        ,m_type("tcp")
        ,m_isStop(true) {
        // SYLAR_LOG_ERROR(g_logger) << sylar::IOManager::GetThis();

        if (m_ioWorker == nullptr)
            SYLAR_LOG_ERROR(g_logger) << "ioWorker指针为空！";
        else
            SYLAR_LOG_INFO(g_logger) << "io指针为" << m_ioWorker;

        if (m_acceptWorker == nullptr)
            SYLAR_LOG_ERROR(g_logger) << "accept指针为空!";
            else
            SYLAR_LOG_INFO(g_logger) << "work指针为："<<m_acceptWorker;
    }


    TcpServer::~TcpServer()
    {
        m_isStop = true;
        for (auto& i : m_socks) {
            // 先取消epoll注册并唤醒等待中的协程，再关闭fd，避免残留上下文
            i->cancelAll();
            i->close();
        }
        m_socks.clear();

        // 正常情况下析构时所有handleClient协程都已结束、m_clients应为空；
        // 这里做个兜底，直接关闭残留的客户端fd
        for (auto& c : m_clients) {
            c->close();
        }
        m_clients.clear();
    }

    void TcpServer::addClient(Socket::ptr client)
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.insert(client);
    }

    void TcpServer::removeClient(Socket::ptr client)
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.erase(client);
    }

    bool TcpServer::bind(sylar::Address::ptr addr)
    {
        std::vector<Address::ptr> addrs;
        std::vector<Address::ptr> fails;
        addrs.push_back(addr);
        return bind(addrs, fails);
    }

    bool TcpServer::bind(const std::vector<Address::ptr>& addrs, std::vector<Address::ptr>& fails)
    {
        for (auto& addr : addrs) {
            Socket::ptr sock = Socket::CreateTCP(addr);
            if (m_reusePort) {
                // 多实例监听同一端口：内核按四元组哈希分发连接，无惊群
                // 注意 fd 在 bind 内部才创建，所以这里只设标志
                sock->setReusePort(true);
            }
            if (!sock->bind(addr)) {
                SYLAR_LOG_ERROR(g_logger) << "bind fail errno=" << errno << " errstr=" << strerror(errno) << " addr=["
                                          << addr->toString() << "]";
                fails.push_back(addr);
                continue;
            }
            // backlog 直接给内核上限（同时把 net.core.somaxconn 调大才真正生效）：
            // 默认 SOMAXCONN(4096) 在连接风暴下会溢出，SYN 被丢后客户端只能靠
            // 指数退避重传，建连速率会断崖式下降
            if (!sock->listen(65535)) {
                SYLAR_LOG_ERROR(g_logger) << "listen fail errno=" << errno << " errstr=" << strerror(errno) << " addr=["
                                          << addr->toString() << "]";
                fails.push_back(addr);
                continue;
            }
            m_socks.push_back(sock);
        }

        if (!fails.empty()) {
            m_socks.clear();
            return false;
        }

        for (auto& i : m_socks) {
            SYLAR_LOG_INFO(g_logger) << "type=" << m_type << " name=" << m_name << " server bind success: " << *i;
        }
        return true;
    }

    Task TcpServer::handleClient(std::shared_ptr<TcpServer> self, Socket::ptr client,
                                 sylar::IOManager* iom)
    {
        (void) self; // 仅用于在协程帧中持有服务器，保证服务器存活
        if (!iom) { iom = m_ioWorker; }   // 未指定则用默认IO实例
        SYLAR_LOG_INFO(g_logger) << "handleClient: " << *client;

        // 接收暂存区放线程局部，不占协程帧空间（每连接省4KB）
        static thread_local char buffer[4096];

        while (!m_isStop) {
            bool peer_closed = false;
            bool recv_error = false;
            bool got_data = false;

            // epoll 读事件是常驻注册的：先主动读一次，读到 EAGAIN 才算没有数据。
            // 这样处理期间到达的数据不会被漏掉，同时避免每次都重新注册 epoll。
            while (true) {
                ssize_t n = client->recv(buffer, sizeof(buffer));
                if (n > 0) {
                    // 二进制安全：按字节数记录，用可打印形式输出，不再追加 '\0'
                    SYLAR_LOG_NOTICE(g_logger) << "服务器收到 " << n << " 字节: "
                                               << to_printable(buffer, n);
                    got_data = true;
                    continue;
                }

                if (n == 0) {
                    if (client->isConnected()) {
                        // Socket::recv 把 EAGAIN 映射为 0；连接仍有效，说明本次数据已读完
                        break;
                    }
                    // 对端关闭连接（EOF）
                    peer_closed = true;
                    break;
                }

                // n < 0：真正的读错误（Socket::recv 对 EAGAIN 返回 0，到不了这里）
                recv_error = true;
                break;
            }

            if (peer_closed) {
                SYLAR_LOG_INFO(g_logger) << "客户端关闭连接";
                break;
            }
            if (recv_error) {
                SYLAR_LOG_ERROR(g_logger) << "recv错误: " << strerror(errno);
                break;
            }
            if (got_data) {
                continue;   // 本轮读到过数据，立刻再检查一次
            }

            // 确实没有数据可读 → 挂起等可读事件
            bool ready = co_await iom->waitReadAsync(client->getSocket(), 5000);
            if (!ready || m_isStop) {
                SYLAR_LOG_INFO(g_logger) << "等待读取超时或服务器停止";
                break;
            }
        }

        // 读事件是常驻注册的：先把fd从epoll摘掉再关闭
        if (iom) { iom->cancelAll(client->getSocket()); }
        client->close();
        removeClient(client);
        SYLAR_LOG_INFO(g_logger) << "连接已关闭";
        co_return;
    }

    Task TcpServer::startAccept(std::shared_ptr<TcpServer> self, Socket::ptr sock)
    {
        (void) self; // 仅用于在协程帧中持有服务器，保证服务器存活
        SYLAR_LOG_INFO(g_logger) << "服务器等待连接...";

        while (!m_isStop) {
            // 监听 fd 的读事件同样是常驻注册：先尝试 accept 排空，
            // 处理期间到达的连接不会被漏掉；确实没有连接时才挂起等事件。
            bool got_conn = false;
            while (true) {
                if (m_isStop) { break; }

                Socket::ptr client = sock->accept();
                if (!client) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break; // 排队的连接收完了
                    }
                    SYLAR_LOG_ERROR(g_logger) << "accept errno=" << errno
                                              << " errstr=" << strerror(errno);
                    break;
                }

                client->setRecvTimeout(m_recvTimeout);
                addClient(client);
                // 分发：pickIoWorker 默认返回 m_ioWorker，子类可重写实现负载均衡
                sylar::IOManager* target = pickIoWorker();
                if (!target) { target = m_ioWorker; }
                target->schedule(handleClient(shared_from_this(), client, target));
                got_conn = true;
            }

            if (m_isStop) { break; }
            if (got_conn) { continue; }   // 收到过连接，立刻再试（可能还有新连接）

            // 没有待处理连接 → 挂起等监听 fd 可读
            bool ready = co_await m_acceptWorker->waitReadAsync(sock->getSocket(), 5000);
            if (!ready || m_isStop) {
                break;
            }
        }
        co_return;
    }

    bool TcpServer::start()
    {
        if (!m_isStop) { return true; }
        m_isStop = false;
        for (auto& sock : m_socks) {
            m_acceptWorker->schedule(startAccept(shared_from_this(), sock));
        }

        return true;
    }

    Task TcpServer::stop(sylar::TcpServer::ptr tcp)
    {
        // 关闭监听socket，唤醒startAccept协程
        for (auto& sock : m_socks) {
            sock->cancelAll();
            sock->close();
        }
        m_socks.clear();

        // 逐个唤醒活动客户端连接的handleClient协程
        std::vector<Socket::ptr> clients;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            clients.assign(m_clients.begin(), m_clients.end());
        }
        for (auto& c : clients) {
            // 只cancel不close：close由被唤醒的handleClient协程自己执行，
            // 避免stop任务和协程并发close同一个socket造成数据竞争
            c->cancelAll();
        }
        co_return;
    }

    void TcpServer::stop() {
        m_isStop = true;
        auto self = shared_from_this();
        m_acceptWorker->schedule(stop(self));
    }

    std::string TcpServer::toString(const std::string& prefix) {
        std::stringstream ss;
        ss << prefix << "[type=" << m_type
           << " name=" << m_name
           << " recv_timeout=" << m_recvTimeout << "]" << std::endl;
        std::string pfx = prefix.empty() ? "    " : prefix;
        for(auto& i : m_socks) {
            ss << pfx << pfx << *i << std::endl;
        }
        return ss.str();
    }


} // namespace sylar
