#pragma once
#include "iomanager.h"
#include "task.h"
#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <vector>
#include <coroutine>

struct redisContext;   // hiredis 前置声明，避免头文件泄漏 C 头

namespace sylar {

/**
 * Redis 异步客户端（限流 + 实时统计）
 * 业务协程只把命令入队，由专职 Redis 线程执行 hiredis 同步命令：
 * - 需要结果的（限流/查询）：协程 co_await 挂起，Redis 线程执行完
 * 通过 IOManager::schedule 唤醒协程；
 * - 不需要结果的（统计）：countAsync 入队即走，fire-and-forget。
 * 队列有上限，满时统计任务丢弃、查询任务降级返回 -1（放行），
 * Redis 挂掉同样降级——保证 Redis 故障不影响服务可用性。
 */
class RedisClient
{
public:
    using ptr = std::shared_ptr<RedisClient>;

    struct RedisAwaiter;   // 前置声明，定义在类尾部

    /**
     * iom: 用于唤醒等待协程的 IOManager
     * maxQueueSize: 队列上限
     */
    explicit RedisClient(IOManager* iom, size_t maxQueueSize = 10000);
    ~RedisClient();

    /**
     * 保存连接参数（真正的 TCP 连接在 Redis 线程里懒建立，不阻塞调用方）
     */
    bool init(const std::string& host = "127.0.0.1", int port = 6379);

    /**
     * 协程版：顺序执行多条命令，返回每条命令的字符串结果
     * 示例: auto r = co_await redis->command({"INCR k", "EXPIRE k 60"});
     */
    RedisAwaiter command(const std::vector<std::string>& cmds);

    /**
     * 异步版：入队即走，不等待结果（统计计数用）
     */
    void countAsync(const std::string& cmd);

    /// 停止 Redis 线程，并唤醒所有还挂起的协程
    void stop();

    /// 队列满时丢弃的任务数
    size_t droppedCount() const { return m_dropped; }

private:
    /// 队列任务：命令 + 结果容器 + 等待的协程
    struct QueueTask
    {
        std::vector<std::string> cmds;                    // 要执行的命令
        std::shared_ptr<std::vector<std::string>> results; // 结果（nullptr=不需要结果）
        std::coroutine_handle<> waiter;                   // 等待的协程（nullptr=无需唤醒）
    };

    void submit(QueueTask t);
    void worker();
    bool ensureConnection();   // 只在 worker 线程调用

    IOManager* m_iom;
    std::string m_host;
    int m_port = 6379;

    std::deque<QueueTask> m_queue;   // 多生产者（工作线程）/ 单消费者（Redis线程）
    std::mutex m_mutex;
    std::condition_variable m_cv;
    size_t m_maxQueueSize;

    std::thread m_thread;
    redisContext* m_ctx = nullptr;   // 只由 Redis 线程访问

    std::atomic<bool> m_stopping{false};
    std::atomic<size_t> m_dropped{0};

public:
    /**
     * 协程等待器：co_await 后挂起，Redis 线程执行完唤醒
     */
    struct RedisAwaiter
    {
        RedisClient* client;
        std::vector<std::string> cmds;
        std::shared_ptr<std::vector<std::string>> results;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> handle)
        {
            // 标记为等待IO：协程挂起后调度器不会把它放回队列空转
            auto th = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
            th.promise().state = Task::WAITING_IO;
            client->submit(QueueTask{std::move(cmds), results, handle});
        }

        const std::vector<std::string>& await_resume() const noexcept { return *results; }
    };
};

} // namespace sylar
