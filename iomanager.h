#pragma once
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>
#include "scheduler.h"
#include "task.h"

namespace sylar {

    class IOManager;

    /**
    * IO事件上下文
    * 每个文件描述符对应一个IO事件上下文
    */
    struct IOContext
    {
        using ptr = std::shared_ptr<IOContext>;

        int fd = -1; //文件描述符
        uint32_t events = 0; //监听的事件类型
        std::coroutine_handle<> handle; // 等待此事件的协程句柄
        IOManager* iomanager = nullptr; //所属的IOManager

        //重置上下文
        void reset()
        {
            fd = -1;
            events = 0;
            handle = nullptr;
            iomanager = nullptr;
        }
    };


    /**
    * IO管理器
    * 基于epoll实现，将IO事件与协程调度结合
    */
    class IOManager : public Scheduler
    {
    public:
        using ptr = std::shared_ptr<IOManager>;

        /**
        * IO事件类型
        */
        enum Event {
            NONE = 0x0, // 无事件
            READ = EPOLLIN, // 读事件
            WRITE = EPOLLOUT, // 写事件
            ERROR = EPOLLERR, // 错误事件
            HUP = EPOLLHUP // 挂起事件
        };


        /**
        * 构造函数
        * threads: 线程数，默认1
        * use_caller: 是否将调用线程也作为工作线程
        */
        explicit IOManager(size_t thread = 1, bool use_caller = true);


        /**
        * 析构函数
        */
        ~IOManager();


        /**
        * 添加事件监听
        * fd: 文件描述符
        * event: 事件类型 (READ/WRITE)
        * cb: 回调函数（用协程代替）
        * 返回: 成功返回0，失败返回-1
        */
        int addEvent(int fd, Event event);


        /**
        * 删除事件监听
        * fd: 文件描述符
        * event: 事件类型
        * 返回: 成功返回0，失败返回-1
        */
        int delEvent(int fd, Event event);


        /**
        * 取消事件监听
        * fd: 文件描述符
        * event: 事件类型
        * 返回: 成功返回0，失败返回-1
        */
        int cancelEvent(int fd, Event event);


        /**
        * 取消所有事件监听
        * fd: 文件描述符
        * 返回: 成功返回0，失败返回-1
        */
        int cancelAll(int fd);

        /**
        * 异步等待fd可读
        * fd: 文件描述符
        * timeout_ms: 超时时间（毫秒），-1表示无限等待
        * 返回: 等待器，可用于co_await
        */
        struct ReadAwaiter
        {
            IOManager* iom;
            int fd;
            int timeout_ms;

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> handle)
            {
                //调用iomanager的waitread函数，传递句柄
                auto th = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
                th.promise().state = Task::WAITING_IO;
                iom->waitRead(fd, timeout_ms, handle);
            }

            bool await_resume() const noexcept { return true; }
        };

        /**
        * 等待fd可读（协程挂起，直到fd可读）
        * fd: 文件描述符
        * timeout_ms: 超时时间（毫秒），-1表示无限等待
        */
        int waitRead(int fd, int timeout_ms = -1, std::coroutine_handle<> handle = {});


        struct WriteAwaiter
        {
            IOManager* iom;
            int fd;
            int timeout_ms;

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> handle)
            {
                //传递句柄
                auto th = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
                th.promise().state = Task::WAITING_IO;
                iom->waitWrite(fd, timeout_ms, handle);
            }

            bool await_resume() const noexcept { return true; }
        };

        /**
        * 等待fd可写（协程挂起，直到fd可写）
        * fd: 文件描述符
        * timeout_ms: 超时时间（毫秒），-1表示无限等待
        */
        int waitWrite(int fd, int timeout_ms = -1, std::coroutine_handle<> handle = {});

        ReadAwaiter waitReadAsync(int fd, int timeous_ms = -1) { return ReadAwaiter{this, fd, timeous_ms};}

        WriteAwaiter waitWriteAsync(int fd, int timeous_ms = -1) { return WriteAwaiter{this,fd,timeous_ms}; }

        /**
        * 等待IOManager停止
        * 协程挂起，直到IOManager::stop()被调用，用于保持任务存活
        */
        struct StopAwaiter
        {
            IOManager* iom;

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> handle)
            {
                // 标记为等待IO，避免调度器把它重新放回队列造成忙转
                auto th = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
                th.promise().state = Task::WAITING_IO;
                iom->waitStop(handle);
            }

            void await_resume() const noexcept {}
        };

        StopAwaiter waitStopAsync() { return StopAwaiter{this}; }

        /**
        * 注册一个等待停止的协程（由waitStopAsync内部调用）
        * handle: 协程句柄
        * 返回: 0表示成功
        */
        int waitStop(std::coroutine_handle<> handle);


        /**
        * 停止IO管理器
        */
        void stop() override;


        /**
        * 获取当前线程的IO管理器
        */
        static IOManager* GetThis();

    protected:

        void idle() override;
        void tickle() override;
        void onWorkerStart() override;

        /**
        * 停止IO管理器（内部实现）
        */
        void stopInner();

        /**
        * 检查是否有待处理事件
        */
        bool hasPendingEvents() const;

    private:
        int m_epfd = -1; //epoll文件描述符
        int m_tickleFds[2] = {-1, -1}; //pipe用于唤醒epoll_wait
        std::atomic<size_t> m_pendingEventCount{0}; //待处理事件数量

        // 读写锁保护m_fdContexts
        mutable std::mutex m_mutex;

        // 串行化epoll_wait：同一时刻只有一个线程等待/处理事件，
        // 避免多个线程并发epoll_wait同一epoll产生"在途事件"导致同一协程被重复调度
        std::mutex m_epollMutex;

        //fd到IOContext的映射
        std::unordered_map<int, IOContext::ptr> m_fdContexts;

        // 保护m_stopHandles
        std::mutex m_stopMutex;
        // 等待停止的协程句柄
        std::vector<std::coroutine_handle<>> m_stopHandles;

        // 线程局部IO管理器
        static thread_local IOManager* t_iomanager;

    };

}   //namespace sylar
