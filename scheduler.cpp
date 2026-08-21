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
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_task.push(std::move(task));
            SYLAR_LOG_DEBUG(SYLAR_LOG_NAME("system")) << "添加任务，队列大小: " << m_task.size();

            if (!is_start)
            {
                is_start = true;

            }
        }

        if(is_start)
            start();

        tickle();
    }

    void Scheduler::start()
    {
        SYLAR_LOG_DEBUG(SYLAR_LOG_NAME("system")) << "执行start函数";
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
            bool has_task = false;

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                if (m_task.empty()) ++m_idleThreadCount;

                //等待条件 ： 有任务或需要停止
                m_cv.wait_for(lock, std::chrono::milliseconds(10),
                              [this]() { return m_stopping || !m_task.empty(); });

                if (m_task.empty()) --m_idleThreadCount;

                //需要停止并且没有任务
                if (m_stopping && m_task.empty())
                {
                    break;
                }

                if (!m_task.empty()) {
                    task = std::move(m_task.front());
                    m_task.pop();
                    has_task = true;
                    SYLAR_LOG_DEBUG(g_logger) << "取出任务，队列剩余: " << m_task.size();
                }
            }


            if (task.getHandle() && has_task)
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
            else
            {
                SYLAR_LOG_DEBUG(g_logger) << "进入idle";
                idle();
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

