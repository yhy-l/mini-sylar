// src/net/socket.cpp
#include "socket.h"
#include "log.h"
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <netinet/tcp.h>

namespace sylar {

    static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

    Socket::ptr Socket::CreateTCP(Address::ptr address) {
        Socket::ptr sock(new Socket(address->getFamily(), TCP, 0));
        return sock;
    }

    Socket::ptr Socket::CreateUDP(Address::ptr address) {
        Socket::ptr sock(new Socket(address->getFamily(), UDP, 0));
        sock->newSock();
        sock->m_isConnected = true;
        return sock;
    }

    Socket::ptr Socket::CreateTCPSocket() {
        Socket::ptr sock(new Socket(IPv4, TCP, 0));
        return sock;
    }

    Socket::ptr Socket::CreateUDPSocket() {
        Socket::ptr sock(new Socket(IPv4, UDP, 0));
        sock->newSock();
        sock->m_isConnected = true;
        return sock;
    }

    Socket::ptr Socket::CreateTCPSocket6() {
        Socket::ptr sock(new Socket(IPv6, TCP, 0));
        return sock;
    }

    Socket::ptr Socket::CreateUDPSocket6() {
        Socket::ptr sock(new Socket(IPv6, UDP, 0));
        sock->newSock();
        sock->m_isConnected = true;
        return sock;
    }

    Socket::ptr Socket::CreateUnixTCPSocket() {
        Socket::ptr sock(new Socket(UNIX, TCP, 0));
        return sock;
    }

    Socket::ptr Socket::CreateUnixUDPSocket() {
        Socket::ptr sock(new Socket(UNIX, UDP, 0));
        sock->newSock();
        sock->m_isConnected = true;
        return sock;
    }

    Socket::Socket(int family, int type, int protocol)
        : m_sock(-1)
        , m_family(family)
        , m_type(type)
        , m_protocol(protocol)
        , m_isConnected(false) {
    }

    Socket::~Socket() {
        close();
    }

    int64_t Socket::getSendTimeout() {
        struct timeval tv;
        if (getOption(SOL_SOCKET, SO_SNDTIMEO, tv)) {
            return tv.tv_sec * 1000 + tv.tv_usec / 1000;
        }
        return -1;
    }

    void Socket::setSendTimeout(int64_t v) {
        struct timeval tv;
        tv.tv_sec = v / 1000;
        tv.tv_usec = (v % 1000) * 1000;
        setOption(SOL_SOCKET, SO_SNDTIMEO, tv);
    }

    int64_t Socket::getRecvTimeout() {
        struct timeval tv;
        if (getOption(SOL_SOCKET, SO_RCVTIMEO, tv)) {
            return tv.tv_sec * 1000 + tv.tv_usec / 1000;
        }
        return -1;
    }

    void Socket::setRecvTimeout(int64_t v) {
        struct timeval tv;
        tv.tv_sec = v / 1000;
        tv.tv_usec = (v % 1000) * 1000;
        setOption(SOL_SOCKET, SO_RCVTIMEO, tv);
    }

    bool Socket::bind(const Address::ptr addr) {
        if (!isValid()) {
            newSock();
            if (!isValid()) {
                return false;
            }
        }

        if (addr->getFamily() != m_family) {
            SYLAR_LOG_ERROR(g_logger) << "bind sock.family("
                                      << m_family << ") addr.family(" << addr->getFamily()
                                      << ") not equal, addr=" << addr->toString();
            return false;
        }

        if (::bind(m_sock, addr->getAddr(), addr->getAddrLen())) {
            SYLAR_LOG_ERROR(g_logger) << "bind error errno=" << errno
                                      << " errstr=" << strerror(errno);
            return false;
        }

        m_localAddress = addr;
        return true;
    }

    bool Socket::bind(const std::string& addr, uint16_t port) {
        // 查找地址
        Address::ptr address = Address::LookupAny(addr);
        if (!address) {
            SYLAR_LOG_ERROR(g_logger) << "地址解析失败: " << addr;
            return false;
        }

        // 尝试转换为IPAddress来设置端口
        IPAddress::ptr ipaddr = std::dynamic_pointer_cast<IPAddress>(address);
        if (ipaddr) {
            // 是IP地址，可以设置端口
            ipaddr->setPort(port);
        } else {
            // 不是IP地址（如Unix地址），无法设置端口
            SYLAR_LOG_WARN(g_logger) << "地址不是IP地址，无法设置端口: " << addr;
        }

        return bind(address);
    }

    bool Socket::listen(int backlog) {
        if (!isValid()) {
            SYLAR_LOG_ERROR(g_logger) << "listen error sock=-1";
            return false;
        }

        if (::listen(m_sock, backlog)) {
            SYLAR_LOG_ERROR(g_logger) << "listen error errno=" << errno
                                      << " errstr=" << strerror(errno);
            return false;
        }

        return true;
    }

    Socket::ptr Socket::accept() {
        if (!isValid()) {
            return nullptr;
        }

        Socket::ptr sock(new Socket(m_family, m_type, m_protocol));

        int newsock = ::accept(m_sock, nullptr, nullptr);
        if (newsock == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞监听下没有待处理连接是正常情况，不算错误
                return nullptr;
            }
            SYLAR_LOG_ERROR(g_logger) << "accept(" << m_sock << ") errno="
                                      << errno << " errstr=" << strerror(errno);
            return nullptr;
        }

        if (sock->init(newsock)) {
            return sock;
        }

        return nullptr;
    }

    bool Socket::connect(const Address::ptr addr) {
        if (!isValid()) {
            newSock();
            if (!isValid()) {
                return false;
            }
        }

        if (addr->getFamily() != m_family) {
            SYLAR_LOG_ERROR(g_logger) << "connect sock.family("
                                      << m_family << ") addr.family(" << addr->getFamily()
                                      << ") not equal, addr=" << addr->toString();
            return false;
        }

        if (::connect(m_sock, addr->getAddr(), addr->getAddrLen())) {
            SYLAR_LOG_ERROR(g_logger) << "sock=" << m_sock << " connect(" << addr->toString()
            << ") error errno=" << errno << " errstr=" << strerror(errno);
            close();
            return false;
        }

        m_isConnected = true;
        m_remoteAddress = addr;
        return true;
    }

    bool Socket::close() {
        if (!m_isConnected && m_sock == -1) {
            return true;
        }

        m_isConnected = false;
        if (m_sock != -1) {
            ::close(m_sock);
            m_sock = -1;
        }
        return true;
    }

    ssize_t Socket::send(const void* buffer, size_t length, int flags) {
        if (!isConnected()) {
            return -1;
        }

        ssize_t bytes = ::send(m_sock, buffer, length, flags);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;  // 非阻塞，暂时无法发送
            }
            SYLAR_LOG_ERROR(g_logger) << "send error errno=" << errno
                                      << " errstr=" << strerror(errno);
        }
        return bytes;
    }

    ssize_t Socket::recv(void* buffer, size_t length, int flags) {
        if (!isConnected()) {
            return -1;
        }

        ssize_t bytes = ::recv(m_sock, buffer, length, flags);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;  // 非阻塞，暂时没有数据
            }
            SYLAR_LOG_ERROR(g_logger) << "recv error errno=" << errno
                                      << " errstr=" << strerror(errno);
        } else if (bytes == 0) {
            // 对端关闭连接
            m_isConnected = false;
        }
        return bytes;
    }

    Address::ptr Socket::getLocalAddress() {
        if (m_localAddress) {
            return m_localAddress;
        }

        Address::ptr result;
        switch (m_family) {
        case AF_INET:
            result.reset(new IPv4Address());
            break;
        case AF_INET6:
            result.reset(new IPv6Address());
            break;
        case AF_UNIX:
            result.reset(new UnixAddress());
            break;
        default:
            result.reset(new UnknownAddress(m_family));
            break;
        }

        socklen_t addrlen = result->getAddrLen();
        if (getsockname(m_sock, result->getAddr(), &addrlen)) {
            SYLAR_LOG_ERROR(g_logger) << "getsockname error sock=" << m_sock
                                      << " errno=" << errno << " errstr=" << strerror(errno);
            return Address::ptr(new UnknownAddress(m_family));
        }

        if (m_family == AF_UNIX) {
            UnixAddress::ptr addr = std::dynamic_pointer_cast<UnixAddress>(result);
            addr->setAddrLen(addrlen);
        }

        m_localAddress = result;
        return m_localAddress;
    }

    Address::ptr Socket::getRemoteAddress() {
        if (m_remoteAddress) {
            return m_remoteAddress;
        }

        Address::ptr result;
        switch (m_family) {
        case AF_INET:
            result.reset(new IPv4Address());
            break;
        case AF_INET6:
            result.reset(new IPv6Address());
            break;
        case AF_UNIX:
            result.reset(new UnixAddress());
            break;
        default:
            result.reset(new UnknownAddress(m_family));
            break;
        }

        socklen_t addrlen = result->getAddrLen();
        if (getpeername(m_sock, result->getAddr(), &addrlen)) {
            SYLAR_LOG_ERROR(g_logger) << "getpeername error sock=" << m_sock
                                      << " errno=" << errno << " errstr=" << strerror(errno);
            return Address::ptr(new UnknownAddress(m_family));
        }

        if (m_family == AF_UNIX) {
            UnixAddress::ptr addr = std::dynamic_pointer_cast<UnixAddress>(result);
            addr->setAddrLen(addrlen);
        }

        m_remoteAddress = result;
        return m_remoteAddress;
    }

    int Socket::getError() {
        int error = 0;
        socklen_t len = sizeof(error);
        if (!getOption(SOL_SOCKET, SO_ERROR, error)) {
            return -1;
        }
        return error;
    }

    std::ostream& Socket::dump(std::ostream& os) const {
        os << "[Socket sock=" << m_sock
           << " is_connected=" << m_isConnected
           << " family=" << m_family
           << " type=" << m_type
           << " protocol=" << m_protocol;
        if (m_localAddress) {
            os << " local_address=" << m_localAddress->toString();
        }
        if (m_remoteAddress) {
            os << " remote_address=" << m_remoteAddress->toString();
        }
        os << "]";
        return os;
    }

    std::string Socket::toString() const {
        std::stringstream ss;
        dump(ss);
        return ss.str();
    }

    bool Socket::cancelRead() {
        IOManager* iom = IOManager::GetThis();
        if (!iom || m_sock == -1) {
            return false;
        }
        return iom->cancelEvent(m_sock, IOManager::READ) == 0;
    }

    bool Socket::cancelWrite() {
        IOManager* iom = IOManager::GetThis();
        if (!iom || m_sock == -1) {
            return false;
        }
        return iom->cancelEvent(m_sock, IOManager::WRITE) == 0;
    }

    bool Socket::cancelAccept() {
        // accept 等待的也是可读事件
        return cancelRead();
    }

    bool Socket::cancelAll() {
        IOManager* iom = IOManager::GetThis();
        if (!iom || m_sock == -1) {
            return false;
        }
        return iom->cancelAll(m_sock) == 0;
    }

    void Socket::initSock() {
        int val = 1;
        setOption(SOL_SOCKET, SO_REUSEADDR, val);
        if (m_type == SOCK_STREAM) {
            setOption(IPPROTO_TCP, TCP_NODELAY, val);
        }
    }

    void Socket::newSock() {
        m_sock = socket(m_family, m_type, m_protocol);
        if (m_sock != -1) {
            initSock();
            // 本框架所有socket都必须非阻塞（epoll驱动，accept/recv/send都不能阻塞线程）
            setNonBlock(true);
        } else {
            SYLAR_LOG_ERROR(g_logger) << "socket(" << m_family
                                      << ", " << m_type << ", " << m_protocol << ") errno="
                                      << errno << " errstr=" << strerror(errno);
        }
    }

    bool Socket::init(int sock) {
        m_sock = sock;
        m_isConnected = true;

        // 设置为非阻塞
        int flags = fcntl(m_sock, F_GETFL, 0);
        if (flags == -1) {
            SYLAR_LOG_ERROR(g_logger) << "fcntl(F_GETFL) errno="
                                      << errno << " errstr=" << strerror(errno);
            return false;
        }

        if (fcntl(m_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
            SYLAR_LOG_ERROR(g_logger) << "fcntl(F_SETFL, O_NONBLOCK) errno="
                                      << errno << " errstr=" << strerror(errno);
            return false;
        }

        initSock();
        getLocalAddress();
        getRemoteAddress();
        return true;
    }

    std::ostream& operator<<(std::ostream& os, const Socket& sock) {
        return sock.dump(os);
    }

    void Socket::setNonBlock(bool nonblock)
    {
        if (!isValid()) { return; }

        int flags = fcntl(m_sock, F_GETFL, 0);
        if (flags < 0) {
            SYLAR_LOG_ERROR(g_logger) << "fcntl F_GETFL失败: " << strerror(errno);
            return;
        }

        if (nonblock) {
            flags |= O_NONBLOCK;
        } else {
            flags &= ~O_NONBLOCK;
        }

        if (fcntl(m_sock, F_SETFL, flags) < 0) {
            SYLAR_LOG_ERROR(g_logger) << "fcntl F_SETFL失败: " << strerror(errno);
            return;
        }
    }

} // namespace sylar
