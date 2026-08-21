// include/net/socket.h
#pragma once
#include <memory>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include "address.h"
#include "iomanager.h"

namespace sylar {

    /**
 * Socket封装类
 */
    class Socket : public std::enable_shared_from_this<Socket> {
    public:
        using ptr = std::shared_ptr<Socket>;
        using weak_ptr = std::weak_ptr<Socket>;

        /**
     * Socket类型
     */
        enum Type {
            TCP = SOCK_STREAM,    // TCP类型
            UDP = SOCK_DGRAM,     // UDP类型
        };

        /**
     * Socket协议簇
     */
        enum Family {
            IPv4 = AF_INET,       // IPv4
            IPv6 = AF_INET6,      // IPv6
            UNIX = AF_UNIX,       // Unix
        };

        /**
     * 创建TCP Socket
     * address地址
     * 返回: 成功返回Socket智能指针，失败返回nullptr
     */
        static Socket::ptr CreateTCP(Address::ptr address);

        /**
     * 创建UDP Socket
     * address地址
     * 返回: 成功返回Socket智能指针，失败返回nullptr
     */
        static Socket::ptr CreateUDP(Address::ptr address);

        /**
     * 创建IPv4的TCP Socket
     */
        static Socket::ptr CreateTCPSocket();

        /**
     * 创建IPv4的UDP Socket
     */
        static Socket::ptr CreateUDPSocket();

        /**
     * 创建IPv6的TCP Socket
     */
        static Socket::ptr CreateTCPSocket6();

        /**
     * 创建IPv6的UDP Socket
     */
        static Socket::ptr CreateUDPSocket6();

        /**
     * 创建Unix的TCP Socket
     */
        static Socket::ptr CreateUnixTCPSocket();

        /**
     * 创建Unix的UDP Socket
     */
        static Socket::ptr CreateUnixUDPSocket();

        /**
     * 构造函数
     * family 地址簇
     * type Socket类型
     * protocol 协议
     */
        Socket(int family, int type, int protocol = 0);

        /**
     * 析构函数
     */
        virtual ~Socket();

        /**
     * 获取发送超时时间(毫秒)
     */
        int64_t getSendTimeout();

        /**
     * 设置发送超时时间(毫秒)
     */
        void setSendTimeout(int64_t v);

        /**
     * 获取接收超时时间(毫秒)
     */
        int64_t getRecvTimeout();

        /**
     * 设置接收超时时间(毫秒)
     */
        void setRecvTimeout(int64_t v);

        /**
     * 获取sockopt
     * level 协议层
     * option 选项名
     * result 返回结果
     * len 结果长度
     * 返回: 是否成功
     */
        template<typename T>
        bool getOption(int level, int option, T& result) {
            socklen_t length = sizeof(T);
            if(getsockopt(m_sock, level, option, &result, &length) != 0) {
                return false;
            }
            return true;
        }

        /**
     * 设置sockopt
     * level 协议层
     * option 选项名
     * value 选项值
     * 返回: 是否成功
     */
        template<typename T>
        bool setOption(int level, int option, const T& value) {
            if(setsockopt(m_sock, level, option, &value, sizeof(T)) != 0) {
                return false;
            }
            return true;
        }

        /**
     * 绑定地址
     * addr 地址
     * 返回: 是否成功
     */
        virtual bool bind(const Address::ptr addr);

        /**
     * 绑定地址
     * addr 地址字符串
     * port 端口
     * 返回: 是否成功
     */
        virtual bool bind(const std::string& addr, uint16_t port = 0);

        /**
     * 监听socket
     * backlog 未完成连接队列的最大长度
     * 返回: 是否成功
     */
        virtual bool listen(int backlog = SOMAXCONN);

        /**
     * 接受连接
     * 返回: 成功返回新连接的socket，失败返回nullptr
     */
        virtual Socket::ptr accept();


        /**
     * 连接到目标地址
     * addr 目标地址
     * 返回: 是否成功
     */
        virtual bool connect(const Address::ptr addr);

        /**
     * 关闭socket
     */
        virtual bool close();

        /**
     * 发送数据
     * buffer 数据缓冲区
     * length 数据长度
     * flags 标志
     * 返回: 发送的字节数，-1表示失败
     */
        virtual ssize_t send(const void* buffer, size_t length, int flags = 0);


        /**
     * 接收数据
     * buffer 接收缓冲区
     * length 缓冲区长度
     * flags 标志
     * 返回: 接收的字节数，-1表示失败
     */
        virtual ssize_t recv(void* buffer, size_t length, int flags = 0);


        /**
     * 获取本地地址
     */
        Address::ptr getLocalAddress();

        /**
     * 获取远端地址
     */
        Address::ptr getRemoteAddress();

        /**
     * 获取socket句柄
     */
        int getSocket() const { return m_sock; }

        /**
     * 获取地址簇
     */
        int getFamily() const { return m_family; }

        /**
     * 获取socket类型
     */
        int getType() const { return m_type; }

        /**
     * 获取协议
     */
        int getProtocol() const { return m_protocol; }

        /**
     * 是否有效
     */
        bool isValid() const { return m_sock != -1; }

        /**
     * 是否连接
     */
        bool isConnected() const { return m_isConnected; }

        /**
     * 获取错误信息
     */
        int getError();

        /**
     * 输出socket信息
     */
        virtual std::ostream& dump(std::ostream& os) const;

        /**
     * 转换为字符串
     */
        std::string toString() const;

        /**
     * 取消读
     */
        bool cancelRead();

        /**
     * 取消写
     */
        bool cancelWrite();

        /**
     * 取消accept
     */
        bool cancelAccept();

        /**
     * 取消所有事件
     */
        bool cancelAll();

    protected:
        /**
     * 初始化socket
     */
        void initSock();

        /**
     * 创建socket
     */
        void newSock();

        /**
     * 初始化socket
     */
        virtual bool init(int sock);

        /**
     * 设置非阻塞锁
     */
        void setNonBlock(bool nonblock = true);


    protected:
        int m_sock;           // socket句柄
        int m_family;         // 地址簇
        int m_type;           // socket类型
        int m_protocol;       // 协议
        bool m_isConnected;   // 是否连接
        Address::ptr m_localAddress;   // 本地地址
        Address::ptr m_remoteAddress;  // 远端地址
    };

    /**
 * 流式输出socket
 */
    std::ostream& operator<<(std::ostream& os, const Socket& sock);

} // namespace sylar
