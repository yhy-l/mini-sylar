#pragma once
#include <memory>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <map>

namespace sylar {

    class IPAddress;

    /**
    * 网络地址的基类,抽象类
    */
    class Address
    {
    public:
        using ptr = std::shared_ptr<Address>;

        /**
        * 通过sockaddr指针创建Address
        * addr sockaddr指针
        * addrlen sockaddr的长度
        * 返回: 返回和sockaddr相匹配的Address,失败返回nullptr
        */
        static Address::ptr Create(const sockaddr *addr, socklen_t addrlen);

        /**
        * 通过host地址返回对应条件的所有Address
        * result 保存满足条件的Address
        * host 域名,服务器名等.举例: www.sylar.top[:80] (方括号为可选内容)
        * family 协议族(AF_INT, AF_INT6, AF_UNIX)
        * type socketl类型SOCK_STREAM、SOCK_DGRAM 等
        * protocol 协议,IPPROTO_TCP、IPPROTO_UDP 等
        * 返回: 返回是否转换成功
        */
        static bool Lookup(std::vector<Address::ptr> &result,
                           const std::string &host,
                           int family = AF_INET,
                           int type = 0,
                           int protocol = 0);

        /**
        * 通过host地址返回对应条件的任意Address
        * host 域名,服务器名等.举例: www.sylar.top[:80] (方括号为可选内容)
        * family 协议族(AF_INT, AF_INT6, AF_UNIX)
        * type socketl类型SOCK_STREAM、SOCK_DGRAM 等
        * protocol 协议,IPPROTO_TCP、IPPROTO_UDP 等
        * 返回: 返回满足条件的任意Address,失败返回nullptr
        */
        static Address::ptr LookupAny(const std::string &host, int family = AF_INET, int type = 0, int protocol = 0);


        /**
        * 通过host地址返回对应条件的任意IPAddress
        * host 域名,服务器名等.举例: www.sylar.top[:80] (方括号为可选内容)
        * family 协议族(AF_INT, AF_INT6, AF_UNIX)
        * type socketl类型SOCK_STREAM、SOCK_DGRAM 等
        * protocol 协议,IPPROTO_TCP、IPPROTO_UDP 等
        * 返回: 返回满足条件的任意IPAddress,失败返回nullptr
        */
        static std::shared_ptr<IPAddress> LookupAnyIPAddress(const std::string &host,
                                                             int family = AF_INET,
                                                             int type = 0,
                                                             int protocol = 0);


        /**
        * 返回本机所有网卡的<网卡名, 地址, 子网掩码位数>
        * result 保存本机所有地址
        * family 协议族(AF_INT, AF_INT6, AF_UNIX)
        * 返回: 是否获取成功
        */
        static bool GetInterfaceAddresses(std::multimap<std::string, std::pair<Address::ptr, uint32_t>> &result,
                                          int family = AF_INET);

        /**
        * 获取指定网卡的地址和子网掩码位数
        * result 保存指定网卡所有地址
        * iface 网卡名称
        * family 协议族(AF_INT, AF_INT6, AF_UNIX)
        * 返回: 是否获取成功
        */
        static bool GetInterfaceAddresses(std::vector<std::pair<Address::ptr, uint32_t>> &result,
                                          const std::string &iface,
                                          int family = AF_INET);


        /**
        * 虚析构函数
        */
        virtual ~Address() {}

        /**
        * 返回协议簇
        */
        int getFamily() const;

        /**
        * 返回sockaddr指针,只读
        */
        virtual const sockaddr *getAddr() const = 0;

        /**
        * 返回sockaddr指针,读写
        */
        virtual sockaddr *getAddr() = 0;

        /**
        * 返回sockaddr的长度
        */
        virtual socklen_t getAddrLen() const = 0;

        /**
        * 可读性输出地址
        */
        virtual std::ostream &insert(std::ostream &os) const = 0;
        //std::ostream无法拷贝，所以需要&进行引用

        /**
         * 返回可读性字符串
        */
        std::string toString() const;

        /**
        * 小于号比较函数
        * true为内存里面更大，或者更长的
        */
        bool operator<(const Address &rhs) const;

        /**
        * 等于函数
        */
        bool operator==(const Address &rhs) const;

        /**
        * 不等于函数
        */
        bool operator!=(const Address &rhs) const;
    };


    /**
    * IP地址的基类
    */
    class IPAddress : public Address
    {
    public:
        using ptr = std::shared_ptr<IPAddress>;

        /**
        * 通过域名,IP,服务器名创建IPAddress
        * address 域名,IP,服务器名等.举例: www.sylar.top
        * port 端口号
        * 返回: 调用成功返回IPAddress,失败返回nullptr
        */
        static IPAddress::ptr Create(const char *address, uint16_t port = 0);

        /**
        * 获取该地址的广播地址
        * prefix_len 子网掩码位数
        * 返回: 调用成功返回IPAddress,失败返回nullptr
        */
        virtual IPAddress::ptr broadcastAddress(uint32_t prefix_len) = 0;

        /**
        * 获取该地址的网段
        * prefix_len 子网掩码位数
        * 返回: 调用成功返回IPAddress,失败返回nullptr
        */
        virtual IPAddress::ptr networkAddress(uint32_t prefix_len) = 0;

        /**
        * 获取子网掩码地址
        * prefix_len 子网掩码位数
        * 返回: 调用成功返回IPAddress,失败返回nullptr
        */
        virtual IPAddress::ptr subnetMask(uint32_t prefix_len) = 0;

        /**
        * 返回端口号
        */
        virtual uint32_t getPort() const = 0;

        /**
        * 设置端口号
        */
        virtual void setPort(uint16_t v) = 0;
    };

    /**
    * IPv4地址
    */
    class IPv4Address : public IPAddress
    {
    public:
        using ptr = std::shared_ptr<IPv4Address>;

        /**
        * 使用点分十进制地址创建IPv4Address
        * address 点分十进制地址,如:192.168.1.1
        * port 端口号
        * 返回: 返回IPv4Address,失败返回nullptr
        */
        static IPv4Address::ptr Create(const char *address, uint16_t port = 0);

        /**
        * 通过sockaddr_in构造IPv4Address
        * address sockaddr_in结构体
        */
        IPv4Address(const sockaddr_in &address);

        /**
        * 通过二进制地址构造IPv4Address
        * address 二进制地址address
        * port 端口号
        */
        IPv4Address(uint32_t address = INADDR_ANY, uint16_t port = 0);

        const sockaddr *getAddr() const override;
        sockaddr *getAddr() override;
        socklen_t getAddrLen() const override;
        std::ostream &insert(std::ostream &os) const override;

        IPAddress::ptr broadcastAddress(uint32_t prefix_len) override;
        IPAddress::ptr networkAddress(uint32_t prefix_len) override;
        IPAddress::ptr subnetMask(uint32_t prefix_len) override;
        uint32_t getPort() const override;
        void setPort(uint16_t v) override;

    private:
        sockaddr_in m_addr;
    };


    /**
    * IPv6地址
    */
    class IPv6Address : public IPAddress
    {
    public:
        using ptr = std::shared_ptr<IPv6Address>;

        /**
        * 通过IPv6地址字符串构造IPv6Address
        * address IPv6地址字符串
        * port 端口号
        */
        static IPv6Address::ptr Create(const char *address, uint16_t port = 0);

        /**
        * 无参构造函数
        */
        IPv6Address();

        /**
        * 通过sockaddr_in6构造IPv6Address
        * address sockaddr_in6结构体
        */
        IPv6Address(const sockaddr_in6 &address);

        /**
        * 通过IPv6二进制地址构造IPv6Address
        * address IPv6二进制地址
        */
        IPv6Address(const uint8_t address[16], uint16_t port = 0);

        const sockaddr *getAddr() const override;
        sockaddr *getAddr() override;
        socklen_t getAddrLen() const override;
        std::ostream &insert(std::ostream &os) const override;

        IPAddress::ptr broadcastAddress(uint32_t prefix_len) override;
        IPAddress::ptr networkAddress(uint32_t prefix_len) override;
        IPAddress::ptr subnetMask(uint32_t prefix_len) override;
        uint32_t getPort() const override;
        void setPort(uint16_t v) override;

    private:
        sockaddr_in6 m_addr;
    };


    /**
    * UnixSocket地址
    */
    class UnixAddress : public Address
    {
    public:
        using ptr = std::shared_ptr<UnixAddress>;

        /**
        * 无参构造函数
        */
        UnixAddress();

        /**
        * 通过路径构造UnixAddress
        * path UnixSocket路径(长度小于UNIX_PATH_MAX)
        */
        UnixAddress(const std::string &path);

        const sockaddr *getAddr() const override;
        sockaddr *getAddr() override;
        socklen_t getAddrLen() const override;
        void setAddrLen(uint32_t v);
        std::string getPath() const;
        std::ostream &insert(std::ostream &os) const override;

    private:
        sockaddr_un m_addr;
        socklen_t m_length;
    };


    /**
    * 未知地址
    */
    class UnknownAddress : public Address {
    public:
        using ptr = std::shared_ptr<UnknownAddress>;
        UnknownAddress(int family);
        UnknownAddress(const sockaddr &addr);
        const sockaddr *getAddr() const override;
        sockaddr *getAddr() override;
        socklen_t getAddrLen() const override;
        std::ostream &insert(std::ostream &os) const override;

    private:
        sockaddr m_addr;
    };

    /**
    * 流式输出Address
    */
    std::ostream &operator<<(std::ostream &os, const Address &addr);

}


