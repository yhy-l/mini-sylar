#pragma once
#include <queue>
#include <memory>
#include <functional>
#include <coroutine>
#include <condition_variable>
#include <atomic>
#include "task.h"
#include "log.h"

namespace sylar {

    /**
     * 协程调度器
     * 管理多个协程任务，按顺序调度执行
     */
    class Scheduler
    {
    public:
        using ptr = std::shared_ptr<Scheduler>;

        /**
         * 构造函数
         * [in]线程数量:
         */
        Scheduler(size_t threads = 2);

        /**
         * 析构函数
         */
        ~Scheduler();

        /**
         * 添加协程任务到调度器
         * [in]: 协程任务
         */
        void schedule(Task task);

        /**
         * 启动调度器
         */
        void start();

        /**
         * 结束调度器
         */
        virtual void stop();

        /**
         * 获得当前线程的调度器
         */
        static Scheduler* GetThis() { return t_scheduler; }

        /**
         * 获取当前调度器是否暂停
         * 
         */
        bool isStopping() const { return m_stopping; }

    protected:
        /**
         * 工作线程启动时的钩子，子类可重写（比如IOManager用来设置线程局部IO管理器）
         */
        virtual void onWorkerStart() {}

        /**
         * 调度器主循环
         */
         void run();

        /**
         * 空闲状态，可被重写
         */
        virtual void idle();

        /**
         * 唤醒空闲状态
         */
        virtual void tickle();

        bool hasIdleThreads() const {return m_idleThreadCount > 0;}

    private:
        std::queue<Task> m_task; //任务队列
        std::mutex m_mutex; //队列锁
        std::condition_variable m_cv; //条件变量
        std::vector<std::thread> m_workers; //工作线程
        std::atomic<bool> m_stopping = false; //停止标志
        bool is_start = false;  //开始标志
        static thread_local Scheduler* t_scheduler; //线程局部调度器
        std::atomic<size_t> m_idleThreadCount{0};   //空闲线程数

        size_t m_threadCount = 2;

    };



}
