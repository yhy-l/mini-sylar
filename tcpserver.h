// include/net/tcp_server.h
#pragma once
#include <memory>
#include <vector>
#include <set>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include "socket.h"
#include "iomanager.h"
#include "log.h"
#include "task.h"

namespace sylar {

    /**
 * TCP服务器类
 */
    class TcpServer : public std::enable_shared_from_this<TcpServer>
    {
    public:
        using ptr = std::shared_ptr<TcpServer>;

        /**
     * 构造函数
     * name 服务器名称
     * type 服务器类型
     * io_worker socket客户端工作的协程调度器
     * accept_worker 服务器socket执行接收socket连接的协程调度器
     */
        TcpServer(sylar::IOManager* io_woker = sylar::IOManager::GetThis()
                  ,sylar::IOManager* accept_worker = sylar::IOManager::GetThis());

        /**
     * 析构函数
     */
        virtual ~TcpServer();

        /**
     * 绑定地址
     * 返回: 返回是否绑定成功
     */
        virtual bool bind(sylar::Address::ptr addr);

        /**
     * 绑定地址数组
     * addrs 需要绑定的地址数组
     * fails 绑定失败的地址
     * 返回: 是否绑定成功
     */
        virtual bool bind(const std::vector<Address::ptr>& addrs
                          ,std::vector<Address::ptr>& fails);

        /**
     * 启动服务
     * 需要bind成功后执行
     */
        virtual bool start();

        /**
     * 停止服务器
     */
        Task stop(sylar::TcpServer::ptr ptr);

        void stop();

        /**
     * 返回读取超时时间(毫秒)
     */
        uint64_t getRecvTimeout() const { return m_recvTimeout;}

        /**
     * 获取服务器名称
     */
        const std::string& getName() const { return m_name; }

        /**
     * 设置读取超时时间(毫秒)
     */
    void setRecvTimeout(uint64_t v) { m_recvTimeout = v;}

    /**
     * 是否允许同一端口多实例监听（SO_REUSEPORT）
     * 开启后内核按连接四元组哈希分发到不同监听 fd，可实现多事件循环并行
     */
    void setReusePort(bool v) { m_reusePort = v; }

        /**
     * 设置服务器名称
     */
        virtual void setName(const std::string& v) { m_name = v;}

    /**
     * 是否停止
     */
        bool isStop() const { return m_isStop;}

        /**
     * 以字符串形式dump server信息
     */
        virtual std::string toString(const std::string& prefix = "");

    protected:
        /**
         * 处理新连接的Socket类
         * self: 持有服务器自身的shared_ptr，保证协程运行期间服务器不会被销毁
         * client: 客户端连接
         */
    /**
     * 处理一个客户端连接
     * iom: 处理该连接的IO实例（可为空，默认用 m_ioWorker）。
     *      支持"accept线程按负载把连接分发给其他IO实例"的架构。
     */
    virtual Task handleClient(std::shared_ptr<TcpServer> self, Socket::ptr client,
                              sylar::IOManager* iom = nullptr);

    /**
     * 选择处理新连接的IO实例（默认用 m_ioWorker，子类可重写实现负载分发）
     */
    virtual sylar::IOManager* pickIoWorker() { return m_ioWorker; }

        /**
         * 开始接受连接
         * self: 持有服务器自身的shared_ptr，保证协程运行期间服务器不会被销毁
         * sock: 监听Socket
         */
        virtual Task startAccept(std::shared_ptr<TcpServer> self, Socket::ptr sock);

        /**
         * 登记一个客户端连接
         */
        void addClient(Socket::ptr client);

        /**
         * 注销一个客户端连接
         */
        void removeClient(Socket::ptr client);

    protected:
        /// 监听Socket数组
        std::vector<Socket::ptr> m_socks;

        /// 活动客户端连接集合（accept成功后登记，连接结束移除）
        std::set<Socket::ptr> m_clients;

        /// 保护m_clients
        std::mutex m_clientsMutex;

        /// 新连接的Socket工作的调度器
        IOManager* m_ioWorker;

        /// 服务器Socket接收连接的调度器
        IOManager* m_acceptWorker;

        /// 接收超时时间(毫秒)
        uint64_t m_recvTimeout;

        /// 服务器名称
        std::string m_name;

        /// 服务器类型
        std::string m_type;

        /// 服务是否停止
    bool m_isStop;

    /// 是否使用 SO_REUSEPORT（多实例监听同一端口）
    bool m_reusePort = false;
    };

} // namespace sylar
