#include "redis_client.h"
#include <hiredis/hiredis.h>
#include <cstdio>
#include <cstring>

namespace sylar {

/// 把 redisReply 转成字符串文本；ARRAY（如 HGETALL）转成 "k1=v1,k2=v2"
static std::string reply_to_string(const redisReply* r)
{
    if (!r) return "-1";
    switch (r->type) {
        case REDIS_REPLY_INTEGER:
            return std::to_string(r->integer);
        case REDIS_REPLY_STRING:
            return std::string(r->str, r->len);
        case REDIS_REPLY_NIL:
            return "";
        case REDIS_REPLY_ARRAY: {
            std::string out;
            for (size_t i = 0; i + 1 < r->elements; i += 2) {
                if (!out.empty()) out += ",";
                const redisReply* k = r->element[i];
                const redisReply* v = r->element[i + 1];
                out += std::string(k->str, k->len);
                out += "=";
                if (v->type == REDIS_REPLY_INTEGER) {
                    out += std::to_string(v->integer);
                } else if (v->type == REDIS_REPLY_STRING) {
                    out += std::string(v->str, v->len);
                }
            }
            return out;
        }
        default:
            return "";
    }
}

RedisClient::RedisClient(IOManager* iom, size_t maxQueueSize)
    : m_iom(iom)
    , m_maxQueueSize(maxQueueSize)
{
    m_thread = std::thread(&RedisClient::worker, this);
}

RedisClient::~RedisClient()
{
    stop();
}

bool RedisClient::init(const std::string& host, int port)
{
    m_host = host;
    m_port = port;
    // 真正的连接在 worker 里懒建立，避免 init 阻塞调用线程
    return true;
}

RedisClient::RedisAwaiter RedisClient::command(const std::vector<std::string>& cmds)
{
    return RedisAwaiter{this, cmds, std::make_shared<std::vector<std::string>>()};
}

void RedisClient::countAsync(const std::string& cmd)
{
    QueueTask t;
    t.cmds.push_back(cmd);   // results/waiter 为空 -> 入队即走
    submit(std::move(t));
}

void RedisClient::submit(QueueTask t)
{
    std::coroutine_handle<> dropWaiter;
    IOManager* dropOwner = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.size() >= m_maxQueueSize) {
            // 满时策略：统计任务丢弃；查询任务降级 -1（放行）并唤醒协程
            ++m_dropped;
            if (t.results) t.results->assign(t.cmds.size(), "-1");
            dropWaiter = t.waiter;
            dropOwner = t.ownerIom;
        } else {
            m_queue.push_back(std::move(t));
            m_cv.notify_one();
            return;
        }
    }
    // 锁外唤醒（不能持 m_mutex 调 schedule，避免锁序交叉）
    if (dropWaiter) {
        (dropOwner ? dropOwner : m_iom)->schedule(Task(dropWaiter));
    }
}

bool RedisClient::ensureConnection()
{
    if (m_ctx) return true;
    m_ctx = redisConnect(m_host.c_str(), m_port);
    if (!m_ctx || m_ctx->err) {
        fprintf(stderr, "[RedisClient] connect %s:%d failed: %s\n",
                m_host.c_str(), m_port, m_ctx ? m_ctx->errstr : "no context");
        if (m_ctx) {
            redisFree(m_ctx);
            m_ctx = nullptr;
        }
        return false;
    }
    return true;
}

void RedisClient::worker()
{
    while (true) {
        QueueTask t;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty() && m_stopping) break;
            t = std::move(m_queue.front());
            m_queue.pop_front();
        }

        // 锁外执行命令（阻塞没关系：这是专职线程）
        bool conn_ok = ensureConnection();
        if (t.results) t.results->resize(t.cmds.size());
        for (size_t i = 0; i < t.cmds.size(); ++i) {
            std::string val = "-1";   // Redis 不可用时降级（放行）
            if (conn_ok) {
                redisReply* reply = static_cast<redisReply*>(
                    redisCommand(m_ctx, t.cmds[i].c_str()));
                if (reply) {
                    val = reply_to_string(reply);
                    freeReplyObject(reply);
                } else {
                    // 连接断了：清理后尝试重连，本批剩余命令降级
                    redisFree(m_ctx);
                    m_ctx = nullptr;
                    conn_ok = false;
                }
            }
            if (t.results) (*t.results)[i] = val;
        }

        // 唤醒等待的协程：交回调度队列，等某个工作线程 resume
        if (t.waiter) {
            (t.ownerIom ? t.ownerIom : m_iom)->schedule(Task(t.waiter));
        }
    }

    // 退出前：唤醒队列里还挂着的协程（关停场景），避免协程帧泄漏
    std::vector<std::pair<std::coroutine_handle<>, IOManager*>> leftovers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty()) {
            if (m_queue.front().results) {
                m_queue.front().results->assign(m_queue.front().cmds.size(), "-1");
            }
            if (m_queue.front().waiter) {
                leftovers.emplace_back(m_queue.front().waiter, m_queue.front().ownerIom);
            }
            m_queue.pop_front();
        }
    }
    for (auto& kv : leftovers) {
        (kv.second ? kv.second : m_iom)->schedule(Task(kv.first));
    }
}

void RedisClient::stop()
{
    m_stopping = true;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_ctx) {
        redisFree(m_ctx);
        m_ctx = nullptr;
    }
}

} // namespace sylar
