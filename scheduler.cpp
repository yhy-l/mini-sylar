#include "scheduler.h"
#include <iostream>

namespace sylar {

    thread_local Scheduler* Scheduler::t_scheduler = nullptr;

    static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

    Scheduler::Scheduler(size_t threads)
        : m_threadCount(threads)
    {
        (void) threads;
    }

    Scheduler::~Scheduler()
    {
        is_start = false;
        stop();
    }

    void Scheduler::schedule(Task task)
    {
        bool need_start = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_task.push(std::move(task));
            SYLAR_LOG_DEBUG(g_logger) << "添加任务，队列大小: " << m_task.size();

            if (!is_start)
            {
                is_start = true;
                need_start = true;
            }
        }

        // 只启动一次工作线程；之前每次schedule都调start()，
        // 会在没有锁保护的情况下读 m_workers（vector），属于数据竞争。
        if (need_start) start();

        // 同线程入队不需要唤醒：调用方正在这个调度器自己的线程上跑，
        // 处理完手上这一轮就会回到任务队列取任务。
        // 只有跨线程投递才需要 tickle()（IOManager 会写唤醒管道唤醒 epoll_wait）。
        // 之前每次入队都唤醒，等于每个请求白搭一次 write 系统调用。
        if (GetThis() != this) tickle();
    }

    void Scheduler::start()
    {
        SYLAR_LOG_DEBUG(g_logger) << "执行start函数";
        // g_logger->setLevel(sylar::LogLevel::DEBUG);
        if (!m_workers.empty()) { return; }

        m_stopping = false;

        //创建工作线程
        for(size_t i = 0; i < m_threadCount; ++i) {
            m_workers.emplace_back([this]() {
                t_scheduler = this;
                onWorkerStart();
                this->run();
            });
        }
    }

    void Scheduler::stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;

        }

        m_cv.notify_all();
        // 工作线程空闲时阻塞在子类的 idle()（IOManager 是 epoll_wait）而不是条件变量上，
        // 所以还要走一次 tickle()，把睡在 epoll_wait 里的线程叫醒，否则 join 会一直等。
        tickle();

        for (auto& worker : m_workers) {
            if (worker.joinable()) { worker.join(); }
        }
        m_workers.clear();
    }

    void Scheduler::run()
    {
        SYLAR_LOG_DEBUG(g_logger) << "调度器开始工作";
        // sylar::g_logger->setLevel(sylar::LogLevel::DEBUG);

        while (true)
        {
            Task task;

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                if (m_task.empty()) {
                    // 队列空且需要停止：退出
                    if (m_stopping) { break; }

                    // 队列空：直接进入空闲等待。
                    // IOManager 的 idle() 会阻塞在 epoll_wait 上，IO 事件一到就返回；
                    // 不能在这里用条件变量睡固定时长（之前是 wait_for 10ms），
                    // 否则每处理完一个请求都会先白等一个超时周期才回去收 IO 事件。
                    //
                    // "我要睡了"必须在锁内登记：此刻之后 schedule() 入队的任务
                    // 会在 tickle() 里看到 idleThreadCount>0 并写唤醒管道，
                    // 不会出现任务已入队却没人被叫醒的丢唤醒。
                    ++m_idleThreadCount;
                    lock.unlock();
                    idle();
                    lock.lock();
                    --m_idleThreadCount;
                    continue;   // 回到顶部重新取任务
                }

                task = std::move(m_task.front());
                m_task.pop();
                SYLAR_LOG_DEBUG(g_logger) << "取出任务，队列剩余: " << m_task.size();
            }


            if (task.getHandle())
            {
                SYLAR_LOG_DEBUG(g_logger) << "执行任务";

                task.resume();

                if (task.done()) {
                    SYLAR_LOG_DEBUG(g_logger) << "任务完成";
                }
                else
                    {SYLAR_LOG_DEBUG(g_logger) << "没有完成";}

                if (!task.done())
                {

                    if(task.getState() == Task::READY)
                    {
                        // 协程主动让出，需要立即重新调度
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_task.push(std::move(task));
                        SYLAR_LOG_DEBUG(g_logger) << "任务主动让出，重新加入队列";
                    }
                    else if(task.getState() == Task::WAITING_IO)
                    {
                        // 协程在等待IO，不要重新加入队列
                        // 等待IO事件触发后，由IOManager重新调度
                        SYLAR_LOG_DEBUG(g_logger) << "任务等待IO，不加入队列";
                    }
                    else
                    {
                        // 其他挂起状态，默认重新入队
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_task.push(std::move(task));
                        SYLAR_LOG_DEBUG(g_logger) << "任务挂起，重新加入队列";
                    }
                }
                else
                {
                    SYLAR_LOG_DEBUG(g_logger) << "任务完成";
                }

            }

        }

    }

    void Scheduler::idle()
    {
        // 默认的空闲实现：短暂睡眠
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    void Scheduler::tickle()
    {

        m_cv.notify_one();
    }


}   //namespace sylar
