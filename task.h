#pragma once
#include <coroutine>
#include <exception>
#include <memory>
#include "log.h"

namespace sylar {

    class Scheduler;

    class Task
    {
    public:
        enum State {
            READY, // 就绪，可以立即执行
            WAITING_IO, // 等待IO事件
            FINISHED // 完成
        };

        struct promise_type
        {
            std::atomic<State> state{READY};
            std::atomic<bool> resuming{false}; // 诊断用：检测并发双重resume

            Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }

            std::suspend_always initial_suspend() noexcept{ return {}; }

            std::suspend_always final_suspend() noexcept{ return {}; }

            void return_void() {state = FINISHED;}

            void unhandled_exception()
            {
                // 协程内未捕获的异常不能静默吞掉：记日志并把协程标记为完成
                state = FINISHED;
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    SYLAR_LOG_ERROR(SYLAR_LOG_NAME("system")) << "协程未捕获异常: " << e.what();
                } catch (...) {
                    SYLAR_LOG_ERROR(SYLAR_LOG_NAME("system")) << "协程未捕获未知异常";
                }
            }

        };

        ~Task()
        {
            if (m_handle && m_handle.done()) {
                SYLAR_LOG_DEBUG(SYLAR_LOG_NAME("system")) << "销毁句柄！！！！";
                m_handle.destroy();
            }
        }

        explicit Task(std::coroutine_handle<promise_type> handle)
            : m_handle(handle)
            , m_scheduler(nullptr)
        {}

        Task() : m_handle(nullptr), m_scheduler(nullptr) {}

        // Task独占协程句柄，禁止拷贝，防止两个Task持有同一句柄导致双重销毁
        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        template<typename PromiseType>
        explicit Task(std::coroutine_handle<PromiseType> handle)
            : m_handle(handle), m_scheduler(nullptr) {}

        // 允许移动
        Task(Task&& other) noexcept : m_handle(other.m_handle), m_scheduler(other.m_scheduler) {
            other.m_handle = nullptr;
            other.m_scheduler = nullptr;
        }

        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                if (m_handle) {
                    if (m_handle.done()) {
                        m_handle.destroy();
                    } else {
                        // 未完成的帧可能仍被IOManager引用（WAITING_IO），不能销毁
                        SYLAR_LOG_WARN(SYLAR_LOG_NAME("system"))
                            << "移动赋值丢弃未完成协程句柄: " << m_handle.address();
                    }
                }
                m_handle = other.m_handle;
                m_scheduler = other.m_scheduler;
                other.m_handle = nullptr;
                other.m_scheduler = nullptr;
            }
            return *this;
        }

        void resume()
        {
            if (m_handle && !m_handle.done())
            {
                auto& pr = std::coroutine_handle<Task::promise_type>::from_address(m_handle.address()).promise();
                if (pr.resuming.exchange(true)) {
                    SYLAR_LOG_FATAL(SYLAR_LOG_NAME("system"))
                        << "检测到并发双重resume! 句柄=" << m_handle.address();
                    std::abort();
                }
                SYLAR_LOG_DEBUG(SYLAR_LOG_NAME("system")) << "恢复协程，句柄: " << m_handle.address();
                auto prev = t_current;
                t_current = m_handle;
                m_handle.resume();
                t_current = prev;
                pr.resuming = false;
            }
            else
            {
                SYLAR_LOG_DEBUG(SYLAR_LOG_NAME("system")) << "无法恢复协程，句柄: " << m_handle.address()
                    << "，完成状态: " << (m_handle ? m_handle.done() : true);
            }
        }

        bool done() const { return !m_handle || m_handle.done();
        }

        /**
         * 设置关联的调度器
        */
        void setScheduler(Scheduler* sched) { m_scheduler = sched;
        }

        /**
        * 获取关联的调度器
        */
        Scheduler* getScheduler() const {
            return m_scheduler;
        }

        /**
        * 获取协程句柄（调度器需要访问）
        */
        std::coroutine_handle<> getHandle() const {
            return m_handle;
        }

        /**
        * 获取当前正在执行的协程
        */
        static std::coroutine_handle<> GetCurrent() {
            return t_current;
        }

        void setState(State s) {
            if (m_handle) {
                std::coroutine_handle<promise_type>::from_address(m_handle.address()).promise().state = s;
            }
        }
        State getState() const {
            if (!m_handle) return READY;
            return std::coroutine_handle<promise_type>::from_address(m_handle.address()).promise().state;
        }

    private:
        std::coroutine_handle<> m_handle;
        Scheduler* m_scheduler = nullptr;

        /// 当前正在执行的协程句柄（线程局部，resume时设置）
        static inline thread_local std::coroutine_handle<> t_current;
    };


}
