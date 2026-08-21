#pragma once
#include "log.h"
#include "mq_log_sender.h"
#include <nlohmann/json.hpp>
#include <deque>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <chrono>

namespace sylar {

/**
 * 异步日志Appender：日志只入队，由后台线程批量写文件
 * 生产者（工作线程）调用log()只做内存入队，不碰磁盘；
 * 后台消费者线程批量取出、格式化、一次性写入文件，避免每条日志一次磁盘IO。
 * 队列有上限，满时丢弃并计数——日志允许丢失，业务线程不能被日志拖垮。
 */
class AsyncLogAppender : public LogAppender
{
public:
    using ptr = std::shared_ptr<AsyncLogAppender>;

    /**
     * file: 日志文件路径
     * maxQueueSize: 队列上限（满时丢弃）
     * batchSize: 每批最多合并多少条日志写一次
     * flushIntervalMs: 空闲时最多等多久唤醒一次（也用于批量攒日志）
     */
    AsyncLogAppender(const std::string& file,
                     size_t maxQueueSize = 10000,
                     size_t batchSize = 100,
                     size_t flushIntervalMs = 100,
                     bool enableMq = true,
                     const std::string& mqHost = "127.0.0.1",
                     int mqPort = 5672,
                     const std::string& mqQueue = "logs",
                     bool writeLocalFile = true);

    ~AsyncLogAppender() override;

    void log(LogEvent::ptr event) override;

    /// 停止后台线程并排空队列（服务器关停时调用）
    void stop();

    /// 已丢弃的日志条数（队列满时）
    size_t droppedCount() const { return m_dropped; }

private:
    void worker();

    std::string m_filename;
    std::ofstream m_filestream;   // 只由后台线程写

    std::deque<LogEvent::ptr> m_queue;   // 有界队列（多生产者/单消费者）
    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::thread m_thread;
    size_t m_maxQueueSize;
    size_t m_batchSize;
    std::chrono::milliseconds m_flushInterval;

    std::unique_ptr<MqLogSender> m_mqSender;   // 可选的RabbitMQ发送器

    std::atomic<bool> m_stopping{false};
    std::atomic<size_t> m_dropped{0};
};

inline AsyncLogAppender::AsyncLogAppender(const std::string& file,
                                          size_t maxQueueSize,
                                          size_t batchSize,
                                          size_t flushIntervalMs,
                                          bool enableMq,
                                          const std::string& mqHost,
                                          int mqPort,
                                          const std::string& mqQueue,
                                          bool writeLocalFile)
    : LogAppender(LogFormatter::ptr(new LogFormatter))
    , m_filename(file)
    , m_maxQueueSize(maxQueueSize)
    , m_batchSize(batchSize)
    , m_flushInterval(std::chrono::milliseconds(flushIntervalMs))
{
    if (writeLocalFile) {
        m_filestream.open(m_filename, std::ios::app);
    }
    if (enableMq) {
        m_mqSender = std::make_unique<MqLogSender>();
        if (!m_mqSender->init(mqHost, mqPort, mqQueue)) {
            fprintf(stderr, "[AsyncLogAppender] RabbitMQ init failed, 仅本地写文件\n");
        }
    }
    m_thread = std::thread(&AsyncLogAppender::worker, this);
}

inline AsyncLogAppender::~AsyncLogAppender()
{
    stop();
}

inline void AsyncLogAppender::log(LogEvent::ptr event)
{
    if (m_stopping) {
        return; // 停止中丢弃；已入队的由worker排空
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.size() >= m_maxQueueSize) {
        ++m_dropped; // 满时策略：丢弃+计数，业务不阻塞
        return;
    }
    m_queue.push_back(std::move(event));
    m_cv.notify_one();
}

inline void AsyncLogAppender::worker()
{
    std::vector<LogEvent::ptr> batch;
    batch.reserve(m_batchSize);

    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // 等到有日志、或到flush间隔、或停止
            m_cv.wait_for(lock, m_flushInterval,
                          [this] { return m_stopping || !m_queue.empty(); });

            // 取一批
            while (!m_queue.empty() && batch.size() < m_batchSize) {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
            }

            if (batch.empty() && m_stopping) {
                break; // 队列排空，退出
            }
        }

        // 锁外批量格式化 + 一次性写入文件
        LogFormatter::ptr fmt = m_formatter ? m_formatter : m_defaultFormatter;
        std::string out;
        std::string mqOut;   // 结构化JSON（多行），用于发送到RabbitMQ
        for (auto& ev : batch) {
            out += fmt->format(ev);

            // 构造结构化JSON：消费者可以按level分文件、统计告警
            nlohmann::json j;
            j["logger"] = ev->getLoggerName();
            j["level"]  = LogLevel::ToString(ev->getLevel());
            j["time"]   = (long long)ev->getTime();
            j["elapse_ms"] = ev->getElapse();
            j["thread"] = ev->getThreadId();
            j["fiber"]  = ev->getFiberId();
            j["file"]   = ev->getFile();
            j["line"]   = ev->getLine();
            j["msg"]    = ev->getContent();
            mqOut += j.dump() + "\n";
        }
        if (!out.empty() && m_filestream.is_open()) {
            m_filestream.write(out.data(), out.size());
            m_filestream.flush();
        }
        // 本地写完后，顺带发到RabbitMQ（失败不影响本地日志）
        if (m_mqSender) {
            m_mqSender->publish(mqOut);
        }
        batch.clear();
    }
}

inline void AsyncLogAppender::stop()
{
    m_stopping = true;
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join(); // 等worker把剩余日志写完
    }
    if (m_filestream.is_open()) {
        m_filestream.flush();
        m_filestream.close();
    }
}

} // namespace sylar
