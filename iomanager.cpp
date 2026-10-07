#include "iomanager.h"
#include "log.h"
#include <cstring>
#include <cerrno>
#include <system_error>

namespace sylar {

    static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

    thread_local IOManager* IOManager::t_iomanager = nullptr;

    IOManager::IOManager(size_t thread, bool use_caller)
        : Scheduler(thread)
    {
        // 创建epoll实例
        m_epfd = epoll_create(5000);
        if (m_epfd < 0) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_create failed: " << strerror(errno);
            throw std::system_error(errno, std::system_category());
        }

        //创建pipe用于唤醒epoll_wait
        if (pipe(m_tickleFds) != 0) {
            SYLAR_LOG_ERROR(g_logger) << "pipe create failed: " << strerror(errno);
            close(m_epfd);
            throw std::system_error(errno, std::system_category());
        }

        // 设置pipe为非阻塞
        fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
        fcntl(m_tickleFds[1], F_SETFL, O_NONBLOCK);

        // 监听pipe的读事件，用于唤醒epoll_wait
        epoll_event event;
        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN | EPOLLET; //边缘触发
        event.data.fd = m_tickleFds[0];

        if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event) != 0) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl add tickle fd failed: " << strerror(errno);
            close(m_tickleFds[0]);
            close(m_tickleFds[1]);
            close(m_epfd);
            throw std::system_error(errno, std::system_category());
        }

        if (use_caller) {
            t_iomanager = this;
            SYLAR_LOG_DEBUG(g_logger) << "设置调用者线程的 IOManager: " << this;
        }

        //启动调度器
        start();
    }

    IOManager::~IOManager()
    {
        stop();
        if (m_epfd > 0) close(m_epfd);
        if (m_tickleFds[0] > 0) close(m_tickleFds[0]);
        if (m_tickleFds[1] > 0) close(m_tickleFds[1]);
    }

    int IOManager::addEvent(int fd, Event event)
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        IOContext::ptr ctx;
        auto it = m_fdContexts.find(fd);
        if (it != m_fdContexts.end()) {
            ctx = it->second;
            if (ctx->events & event) {
                // 事件已经存在
                SYLAR_LOG_ERROR(g_logger) << "addEvent fd=" << fd << " event=" << event << " already exists";
                return -1;
            }
            ctx->events = (Event) (ctx->events | event);
        } else {
            ctx = std::make_shared<IOContext>();
            ctx->fd = fd;
            ctx->events = event;
            ctx->iomanager = this;
            m_fdContexts[fd] = ctx;
        }

        //配置epoll事件
        epoll_event epevent;
        memset(&epevent, 0, sizeof(epevent));
        epevent.events = EPOLLET | ctx->events;
        epevent.data.fd = fd;

        int op = it != m_fdContexts.end() ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (epoll_ctl(m_epfd, op, fd, &epevent) != 0) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(" << op << ") fd=" << fd << " failed: " << strerror(errno);
            if (it == m_fdContexts.end()) { m_fdContexts.erase(fd); }
            return -1;
        }
        ++m_pendingEventCount;
        return 0;
    }

    int IOManager::delEvent(int fd, Event event)
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        auto it = m_fdContexts.find(fd);
        if (it == m_fdContexts.end()) {
            SYLAR_LOG_ERROR(g_logger) << "未找到fd,删除事件失败";
            return -1;
        }

        IOContext::ptr ctx = it->second;
        if (!(ctx->events & event)) {
            SYLAR_LOG_ERROR(g_logger) << "未找到event,删除失败";
            return -1;
        }

        Event new_events = (Event) (ctx->events & ~event);
        if (new_events == NONE) {
            if (epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd, nullptr) != 0) {
                SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(DEL) fd=" << fd << " failed: " << strerror(errno);
                return -1;
            }
            m_fdContexts.erase(it);
        } else {
            ctx->events = new_events;
            epoll_event epevent;
            memset(&epevent, 0, sizeof(epevent));
            epevent.events = EPOLLET | ctx->events;
            epevent.data.fd = fd;

            if (epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd, &epevent) != 0) {
                SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(MOD) fd=" << fd << " failed: " << strerror(errno);
                return -1;
            }
        }

        --m_pendingEventCount;
        return 0;
    }

    int IOManager::cancelEvent(int fd, Event event)
    {
        std::coroutine_handle<> handle;
        {
            std::unique_lock<std::mutex> lock(m_mutex);

            auto it = m_fdContexts.find(fd);
            if (it == m_fdContexts.end()) {
                SYLAR_LOG_ERROR(g_logger) << "未找到fd,取消事件失败";
                return -1;
            }

            IOContext::ptr ctx = it->second;

            // 取出等待中的协程句柄，锁外再恢复，避免持锁调用schedule
            if (ctx->handle) {
                auto th = std::coroutine_handle<Task::promise_type>::from_address(ctx->handle.address());
                if (th.promise().state == Task::WAITING_IO && !th.promise().resuming) {
                    handle = ctx->handle;
                    ctx->handle = nullptr;
                }
                // 协程正在运行则不动句柄（DEL+erase照常，运行中的协程会自己重新注册或退出）
            }

            if (epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd, nullptr) != 0) {
                SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(DEL) fd=" << fd << " failed: " << strerror(errno);
            }

            m_fdContexts.erase(it);
            --m_pendingEventCount;
        }

        if (handle) {
            auto task_handle = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
            task_handle.promise().state = Task::READY;
            schedule(Task(handle));
        }
        return 0;
    }

    int IOManager::cancelAll(int fd)
    {
        std::coroutine_handle<> handle;
        {
            std::unique_lock<std::mutex> lock(m_mutex);

            auto it = m_fdContexts.find(fd);
            if (it == m_fdContexts.end()) {
                // 连接正常收尾时上下文可能已经没了，属于常见情况，不当作错误
                SYLAR_LOG_DEBUG(g_logger) << "未找到fd,取消事件失败";
                return -1;
            }

            IOContext::ptr ctx = it->second;

            // 取出等待中的协程句柄，锁外再恢复，避免持锁调用schedule
            if (ctx->handle) {
                auto th = std::coroutine_handle<Task::promise_type>::from_address(ctx->handle.address());
                if (th.promise().state == Task::WAITING_IO && !th.promise().resuming) {
                    handle = ctx->handle;
                    ctx->handle = nullptr;
                }
                // 协程正在运行则不动句柄（DEL+erase照常，运行中的协程会自己重新注册或退出）
            }

            if (epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd, nullptr) != 0) {
                SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(DEL) fd=" << fd << " failed: " << strerror(errno);
            }

            m_fdContexts.erase(it);
            --m_pendingEventCount;
        }

        if (handle) {
            auto task_handle = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
            task_handle.promise().state = Task::READY;
            schedule(Task(handle));
        }
        return 0;
    }

    int IOManager::waitRead(int fd, int timeout_ms, std::coroutine_handle<> handle)
    {
        SYLAR_LOG_DEBUG(g_logger) << "进入waitRead";

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_fdContexts.find(fd);
        if (it != m_fdContexts.end()) {
            IOContext::ptr ctx = it->second;
            if (ctx->waitEvents & READ) {
                // 已经有协程在等这个fd的读事件
                SYLAR_LOG_ERROR(g_logger) << "waitRead fd=" << fd << " already waiting";
                return -1;
            }

            if (ctx->handle) {
                SYLAR_LOG_WARN(g_logger) << "fd " << fd << " already has a waiting coroutine, replacing";
            }
            ctx->handle = handle;
            ctx->waitEvents |= READ;

            if (ctx->events & READ) {
                // 读事件常驻注册：epoll里已经挂着这个fd，直接复用，
                // 不需要再走一次 epoll_ctl（每请求省一次系统调用）
                SYLAR_LOG_DEBUG(g_logger) << "复用已注册的读事件 fd=" << fd;
                return 0;
            }

            ctx->events |= READ;
            epoll_event epevent;
            memset(&epevent, 0, sizeof(epevent));
            epevent.events = EPOLLET | ctx->events;
            epevent.data.fd = fd;
            if (epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd, &epevent) != 0) {
                SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(MOD) fd=" << fd << " failed";
                ctx->handle = nullptr;
                ctx->waitEvents = 0;
                return -1;
            }
            return 0;
        }

        IOContext::ptr ctx = std::make_shared<IOContext>();
        ctx->fd = fd;
        ctx->events = READ;
        ctx->waitEvents = READ;
        ctx->iomanager = this;
        ctx->handle = handle;
        m_fdContexts[fd] = ctx;

        //配置epoll
        epoll_event epevent;
        memset(&epevent, 0, sizeof(epevent));
        epevent.events = EPOLLET | ctx->events;
        epevent.data.fd = fd;

        if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, fd, &epevent) != 0) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(ADD) fd=" << fd << " failed";
            m_fdContexts.erase(fd);
            ctx->handle = nullptr;
            return -1;
        }
        ++m_pendingEventCount;

        SYLAR_LOG_DEBUG(g_logger) << "waitRead fd=" << fd << ", handle=" << handle.address();
        return 0;
    }

    int IOManager::waitWrite(int fd, int timeout_ms, std::coroutine_handle<> handle)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_fdContexts.find(fd);
        if (it != m_fdContexts.end()) {
            IOContext::ptr ctx = it->second;
            if (ctx->waitEvents & WRITE) {
                SYLAR_LOG_ERROR(g_logger) << "waitWrite fd=" << fd << " already waiting";
                return -1;
            }

            if (ctx->handle) {
                SYLAR_LOG_WARN(g_logger) << "fd " << fd << " already has a waiting coroutine, replacing";
            }
            ctx->handle = handle;
            ctx->waitEvents |= WRITE;

            if (ctx->events & WRITE) {
                // 写关注已经注册过了，直接复用
                return 0;
            }

            ctx->events |= WRITE;
            epoll_event epevent;
            memset(&epevent, 0, sizeof(epevent));
            epevent.events = EPOLLET | ctx->events;
            epevent.data.fd = fd;
            if (epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd, &epevent) != 0) {
                SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(MOD) fd=" << fd << " failed";
                ctx->handle = nullptr;
                ctx->waitEvents = 0;
                return -1;
            }
            return 0;
        }

        IOContext::ptr ctx = std::make_shared<IOContext>();
        ctx->fd = fd;
        ctx->events = WRITE;
        ctx->waitEvents = WRITE;
        ctx->iomanager = this;
        ctx->handle = handle;
        m_fdContexts[fd] = ctx;

        // 配置epoll事件
        epoll_event epevent;
        memset(&epevent, 0, sizeof(epevent));
        epevent.events = EPOLLET | ctx->events;
        epevent.data.fd = fd;

        if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, fd, &epevent) != 0) {
            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl(ADD) fd=" << fd << " failed";
            m_fdContexts.erase(fd);
            ctx->handle = nullptr;
            return -1;
        }
        ++m_pendingEventCount;

        SYLAR_LOG_DEBUG(g_logger) << "waitWrite fd=" << fd << ", handle=" << handle.address();
        return 0;
    }

    int IOManager::waitStop(std::coroutine_handle<> handle)
    {
        std::lock_guard<std::mutex> lock(m_stopMutex);

        if (isStopping()) {
            // 已经在停止，立即恢复协程
            schedule(Task(handle));
            return 0;
        }

        m_stopHandles.push_back(handle);
        return 0;
    }

    void IOManager::stop()
    {
        // 恢复所有等待停止的协程，让它们能正常结束并释放持有的对象
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            for (auto& handle : m_stopHandles) {
                auto th = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
                th.promise().state = Task::READY;
                schedule(Task(handle));
            }
            m_stopHandles.clear();
        }

        stopInner();
        Scheduler::stop();
    }

    void IOManager::stopInner()
    {
        // 唤醒所有等待的线程
        if (m_tickleFds[1] >= 0) {
            char buf[] = "T";
            if (write(m_tickleFds[1], buf, sizeof(buf)) < 0) {
                // 关闭过程中管道写失败可忽略
            }
        }
    }

    IOManager* IOManager::GetThis()
    {
        return t_iomanager;
    }

    void IOManager::onWorkerStart()
    {
        // 让工作线程也能通过 GetThis() 拿到所属的IOManager（Socket::cancelAll等会用到）
        t_iomanager = this;
    }

    bool IOManager::hasPendingEvents() const
    {
        return m_pendingEventCount > 0;
    }

    bool IOManager::takePendingEvent(int fd, Event event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_fdContexts.find(fd);
        if (it == m_fdContexts.end()) {
            return false;
        }

        if (!(it->second->pendingEvents & event)) {
            return false;
        }

        it->second->pendingEvents &= ~event;
        return true;
    }

    void IOManager::idle()
    {
        static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

        // g_logger->setLevel(sylar::LogLevel::DEBUG);
        SYLAR_LOG_DEBUG(g_logger) << "IOManager进入空闲，等待IO事件";

        // 同一时刻只允许一个线程epoll_wait+处理事件批次。
        // 否则多个线程并发epoll_wait同一epoll时，同一fd的多个事件会落在不同线程，
        // 产生"在途事件"——协程重新注册后，在途事件按fd找到新上下文再次调度同一协程，
        // 导致同一协程帧被多个线程并发resume（堆损坏）。
        std::lock_guard<std::mutex> epollLock(m_epollMutex);

        if (isStopping()) {
            return; // 停止中不再等待事件，让工作线程尽快退出
        }

        const int MAX_EVENTS = 256;
        epoll_event events[MAX_EVENTS];

        // 阻塞在epoll_wait上,发挥结果保存到events上，最大可以返回MAX_EVENTS个events
        int n = epoll_wait(m_epfd, events, MAX_EVENTS, 1000); // 1秒超时

        if (n < 0) {
            if (errno == EINTR) {
                return; // 被信号中断
            }
            SYLAR_LOG_ERROR(g_logger) << "epoll_wait错误: " << strerror(errno);
            return;
        }

        if (n == 0) {
            SYLAR_LOG_DEBUG(g_logger) << "epoll_wait超时，无事件";
            return;
        }

        // 处理IO事件
        for (int i = 0; i < n; i++){
            epoll_event& event = events[i];

            // 检查是否是唤醒事件
            if (event.data.fd == m_tickleFds[0]) {
                SYLAR_LOG_DEBUG(g_logger) << "接收到唤醒信号";
                char buf[256];
                while (read(m_tickleFds[0], buf, sizeof(buf)) > 0) {}
                continue;
            }

            // 把要唤醒的协程句柄取出来，稍后在锁外恢复
            std::coroutine_handle<> handle;
            {
                // epoll事件里只存fd不存指针：上下文可能在epoll_wait返回后被其他线程清理释放，
                // 所以必须在锁内从map现查当前上下文，保证指针存活
                std::lock_guard<std::mutex> lock(m_mutex);

                int fd = event.data.fd;
                auto it = m_fdContexts.find(fd);
                if (it == m_fdContexts.end()) {
                    // fd已被清理（可能是过期事件），忽略
                    SYLAR_LOG_DEBUG(g_logger) << "忽略无上下文的IO事件: fd=" << fd;
                    continue;
                }
                IOContext* ctx = it->second.get();

                // 确定触发的事件类型：对端关闭/挂起也算"可读"，让协程去recv拿到EOF
                uint32_t happened_events = 0;
                if (event.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
                    happened_events |= READ;
                    SYLAR_LOG_DEBUG(g_logger) << "IO事件: fd=" << ctx->fd << " 可读";
                }
                if (event.events & EPOLLOUT) {
                    happened_events |= WRITE;
                    SYLAR_LOG_DEBUG(g_logger) << "IO事件: fd=" << ctx->fd << " 可写";
                }
                if (event.events & EPOLLERR) {
                    happened_events |= (READ | WRITE);
                }

                bool need_rearm = false;

                // 只有等待者确实在等这个事件时才唤醒它
                if (ctx->handle && (happened_events & ctx->waitEvents)) {
                    auto th = std::coroutine_handle<Task::promise_type>::from_address(ctx->handle.address());
                    // resuming=true表示协程正在被某个线程resume（含await_suspend阶段），
                    // 此时取句柄会导致第二个线程并发resume同一帧。必须等resume完全返回。
                    if (th.promise().state == Task::WAITING_IO && !th.promise().resuming) {
                        handle = ctx->handle;
                        ctx->handle = nullptr;
                        ctx->waitEvents = 0;
                    } else {
                        // 过期/在途事件：协程已被调度或正在运行，不能重复取句柄。
                        // 直接跳过整个事件、不动上下文（协程正在用它的注册状态）。
                        SYLAR_LOG_DEBUG(g_logger) << "跳过在途事件 fd=" << ctx->fd;
                        continue;
                    }
                } else if (happened_events) {
                    // 事件到了但没有等待者（或等待者等的不是这个事件，比如在等写却来了读）：
                    // 记下来，等它下次挂起时由 await_ready 直接返回就绪，避免丢事件
                    ctx->pendingEvents |= happened_events;
                }

                // 写事件是一次性关注：触发后取消注册，由等待者需要时重新注册
                if ((ctx->events & WRITE) && (happened_events & WRITE)) {
                    ctx->events &= ~WRITE;
                    need_rearm = true;
                }
                // 读事件是常驻注册：触发后保留在epoll里，下次 waitRead 直接复用，
                // 省掉每请求一次 epoll_ctl(ADD/DEL)；连接结束时由 cancelAll 统一摘除。

                if (need_rearm) {
                    if (ctx->events != NONE) {
                        epoll_event epevent;
                        memset(&epevent, 0, sizeof(epevent));
                        epevent.events = EPOLLET | ctx->events;
                        epevent.data.fd = ctx->fd;

                        if (epoll_ctl(m_epfd, EPOLL_CTL_MOD, ctx->fd, &epevent) != 0) {
                            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl修改失败 fd=" << ctx->fd;
                        }
                    } else {
                        // 没有任何关注事件了：从epoll删除并清理map
                        int efd = ctx->fd;
                        if (epoll_ctl(m_epfd, EPOLL_CTL_DEL, efd, nullptr) != 0) {
                            SYLAR_LOG_ERROR(g_logger) << "epoll_ctl删除失败 fd=" << efd;
                        }
                        m_fdContexts.erase(it);
                        --m_pendingEventCount;
                    }
                }
            }

            // 锁外恢复协程（schedule需要拿调度器队列锁，不能和m_mutex交叉持有）
            if (handle) {
                SYLAR_LOG_DEBUG(g_logger) << "恢复等待的协程: " << handle.address();

                auto task_handle = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
                task_handle.promise().state = Task::READY;

                schedule(Task(handle));
            }
        }
    }

    void IOManager::tickle() {
        if (!hasIdleThreads()) {
            return;
        }


        SYLAR_LOG_DEBUG(g_logger) << "发送唤醒信号";
        char buf = 'T';
        if (write(m_tickleFds[1], &buf, 1) != 1) {
            SYLAR_LOG_ERROR(g_logger) << "写唤醒管道失败";
        }
    }

}
