#pragma once
#include "log.h"
#include "mq_log_sender.h"
#include <nlohmann/json.hpp>
#include <array>
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
 * 异步日志Appender：双缓冲 + 后台线程批量写文件
 * 生产者（工作线程）调用log()只把日志指针写入"当前缓冲"（互斥锁保护，预分配无堆分配）；
 * 缓冲写满或后台线程定时触发时交换两块缓冲，后台线程锁外整块格式化、写盘、发MQ。
 * 两块缓冲都忙时丢弃并计数——日志允许丢失，业务线程不能被日志拖垮。
 */
class AsyncLogAppender : public LogAppender
{
public:
    using ptr = std::shared_ptr<AsyncLogAppender>;

    /**
     * file: 日志文件路径
     * maxQueueSize: 每块缓冲容量（满时交换；两块都忙则丢弃）
     * batchSize: 保留参数（兼容旧调用，双缓冲下不再使用）
     * flushIntervalMs: 空闲时最多等多久触发一次落盘
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

    std::array<std::vector<LogEvent::ptr>, 2> m_buf;  // 双缓冲：一块业务写入、一块后台落盘
    std::array<bool, 2> m_busy{false, false};         // true=该块已交给后台线程（业务不可写）
    int m_cur = 0;                                    // 业务当前写入的块索引
    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::thread m_thread;
    size_t m_capacity;
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
    , m_capacity(maxQueueSize)
    , m_flushInterval(std::chrono::milliseconds(flushIntervalMs))
{
    m_buf[0].reserve(m_capacity);
    m_buf[1].reserve(m_capacity);
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
        return; // 停止中丢弃；已入缓冲的由worker排空
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_buf[m_cur].size() >= m_capacity) {
        int other = 1 - m_cur;
        if (m_busy[other]) {
            ++m_dropped; // 两块都忙：丢弃+计数，业务不阻塞
            return;
        }
        // 当前块写满：交给后台线程，切换到另一块继续写
        m_busy[m_cur] = true;
        m_cur = other;
        m_cv.notify_one();
    }
    m_buf[m_cur].push_back(std::move(event));   // 已 reserve，不触发堆分配
}

inline void AsyncLogAppender::worker()
{
    LogFormatter::ptr fmt = m_formatter ? m_formatter : m_defaultFormatter;

    while (true) {
        int take = -1;      // 要处理的块索引
        size_t count = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // 等到有满块可处理、或到flush间隔、或停止
            m_cv.wait_for(lock, m_flushInterval,
                          [this] { return m_stopping || m_busy[0] || m_busy[1]; });

            // 空闲定时（或停止时）：把当前写块切出，保证低流量日志也及时落盘
            int other = 1 - m_cur;
            if (m_buf[m_cur].size() > 0 && !m_busy[other]) {
                m_busy[m_cur] = true;
                m_cur = other;
            }

            if (m_busy[0]) take = 0;
            else if (m_busy[1]) take = 1;
            if (take >= 0) count = m_buf[take].size();

            if (take < 0 && m_stopping) {
                break; // 队列排空，退出
            }
        }

        if (take < 0) continue;

        // 锁外整块格式化 + 一次性写入文件 + 发MQ
        std::string out;
        std::string mqOut;   // 结构化JSON（多行），用于发送到RabbitMQ
        size_t mqCount = 0;
        const size_t MQ_CHUNK = 200;   // MQ分块发送，避免单条消息过大
        for (size_t i = 0; i < count; ++i) {
            LogEvent::ptr& ev = m_buf[take][i];
            out += fmt->format(ev);

            if (m_mqSender) {
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
                if (++mqCount >= MQ_CHUNK) {
                    m_mqSender->publish(mqOut);
                    mqOut.clear();
                    mqCount = 0;
                }
            }
        }
        if (!out.empty() && m_filestream.is_open()) {
            m_filestream.write(out.data(), out.size());
            m_filestream.flush();
        }
        if (m_mqSender && !mqOut.empty()) {
            m_mqSender->publish(mqOut);
        }

        // 归还缓冲块（置空并标记空闲，供业务再次写入）
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_buf[take].clear();
            m_busy[take] = false;
        }
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
