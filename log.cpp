#include "log.h"
#include "async_log_appender.h"
#include <utility>
#include <chrono>
#include <functional>
#include <cstdlib>

namespace sylar {

    const char *LogLevel::ToString(Level level)
    {
        switch (level) {
#define XX(name) \
    case LogLevel::name: \
        return #name;
            XX(FATAL);
            XX(ALERT);
            XX(CRIT);
            XX(ERROR);
            XX(WARN);
            XX(NOTICE);
            XX(INFO);
            XX(DEBUG);
#undef XX
        default:
            return "NOTSET";
        }
        return "NOTSET";
    }

    LogLevel::Level LogLevel::FromString(const std::string &str)
    {
#define XX(level, v) \
    if (str == #v) { return LogLevel::level; }
        XX(FATAL, fatal);
        XX(ALERT, alert);
        XX(CRIT, crit);
        XX(ERROR, error);
        XX(WARN, warn);
        XX(NOTICE, notice);
        XX(INFO, info);
        XX(DEBUG, debug);

        XX(FATAL, FATAL);
        XX(ALERT, ALERT);
        XX(CRIT, CRIT);
        XX(ERROR, ERROR);
        XX(WARN, WARN);
        XX(NOTICE, NOTICE);
        XX(INFO, INFO);
        XX(DEBUG, DEBUG);
#undef XX

        return LogLevel::NOTICE;
    }

    LogEvent::LogEvent(const std::string &logger_name,
                       LogLevel::Level level,
                       const char *file,
                       int32_t line,
                       int64_t elapse,
                       uint32_t thread_id,
                       uint64_t fiber_id,
                       time_t time,
                       const std::string &thread_name)
        : m_level(level)
        , m_file(file)
        , m_line(line)
        , m_elapse(elapse)
        , m_threadId(thread_id)
        , m_fiberId(fiber_id)
        , m_time(time)
        , m_threadName(thread_name)
        , m_loggerName(logger_name)
    {}

    LogFormatter::LogFormatter(const std::string &pattern)
        : m_pattern(pattern)
    {
        init();
    }

    /**
     * 内容输出
     */
    class MessageFormatItem : public LogFormatter::FormatItem
    {
    public:
        MessageFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << event->getContent(); }
    };

    /**
     * 等级输出
     */
    class LevelFormatItem : public LogFormatter::FormatItem
    {
    public:
        LevelFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << LogLevel::ToString(event->getLevel()); }
    };

    /**
     * 运行时间输出
     */
    class ElapseFormatItem : public LogFormatter::FormatItem
    {
    public:
        ElapseFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << event->getElapse(); }
    };

    /**
     * 日志名字输出
     */
    class LoggerNameFormatItem : public LogFormatter::FormatItem
    {
    public:
        LoggerNameFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << event->getLoggerName(); }
    };

    /**
     * 线程id输出
     */
    class ThreadIdFormatItem : public LogFormatter::FormatItem
    {
    public:
        ThreadIdFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << event->getThreadId(); }
    };

    /**
     * 协程id输出
     */
    class FiberIdFormatItem : public LogFormatter::FormatItem
    {
    public:
        FiberIdFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << event->getFiberId(); }
    };

    /**
     * 线程名字输出
     */
    class ThreadNameFormatItem : public LogFormatter::FormatItem
    {
    public:
        ThreadNameFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << event->getThreadName(); }
    };

    /**
     * 格式输出
     */
    class DateTimeFormatItem : public LogFormatter::FormatItem
    {
    public:
        DateTimeFormatItem(const std::string &format = "%Y-%m-%d %H:%M:%S")
            : m_format(format)
        {
            if (m_format.empty()) { m_format = "%Y-%m-%d %H:%M:%S"; }
        }

        void format(std::ostream &os, LogEvent::ptr event) override
        {
            struct tm tm;
            time_t time = event->getTime();
            localtime_r(&time, &tm);
            char buf[64];
            strftime(buf, sizeof(buf), m_format.c_str(), &tm);
            os << buf;
        }

    private:
        std::string m_format;
    };

    /**
     * 文件输出
     */
    class FileNameFormatItem : public LogFormatter::FormatItem
    {
    public:
        FileNameFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << event->getFile(); }
    };

    /**
     * 行号输出
     */
    class LineFormatItem : public LogFormatter::FormatItem
    {
    public:
        LineFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << event->getLine(); }
    };

    /**
     * 输出换行
     */
    class NewLineFormatItem : public LogFormatter::FormatItem
    {
    public:
        NewLineFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << std::endl; }
    };

    /**
     * 自定义换行
     */
    class StringFormatItem : public LogFormatter::FormatItem
    {
    public:
        StringFormatItem(const std::string &str)
            : m_string(str)
        {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << m_string; }

    private:
        std::string m_string;
    };

    /**
     * 输出tab空行
     */
    class TabFormatItem : public LogFormatter::FormatItem
    {
    public:
        TabFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << "  "; }
    };

    /**
     * 输出%
     */
    class PercentSignFormatItem : public LogFormatter::FormatItem
    {
    public:
        PercentSignFormatItem(const std::string &str) {}
        void format(std::ostream &os, LogEvent::ptr event) override { os << "%"; }
    };

    void LogFormatter::init()
    {
        // 存放格式
        std::vector<std::pair<int, std::string>> patterns;
        // 临时存储常规字符串
        std::string tmp;
        // 日期格式字符串
        std::string dateformat;
        // 是否解析出错
        bool error = false;

        // 是否正在解析常规字符，初始时为true
        bool parsing_string = true;

        size_t i = 0;
        while (i < m_pattern.size()) {
            std::string c = std::string(1, m_pattern[i]);

            if (c == "%") {
                if (parsing_string) {
                    if (!tmp.empty()) { patterns.push_back(std::make_pair(0, tmp)); }
                    tmp.clear();
                    parsing_string = false; // 在解析常规字符时遇到%，表示开始解析模板字符
                    i++;
                    continue;
                } else {
                    patterns.push_back(std::make_pair(1, c));
                    parsing_string = true; // 在解析模板字符时遇到%，表示这里是一个%转义
                    i++;
                    continue;
                }
            } else // not %
            {
                if (parsing_string) { // 持续解析常规字符直到遇到%，解析出的字符串作为一个常规字符串加入patterns
                    tmp += c;
                    i++;
                    continue;
                } else { // 模板字符，直接添加到patterns中，添加完成后，状态变为解析常规字符，%d特殊处理
                    patterns.push_back(std::make_pair(1, c));
                    parsing_string = true;

                    // 后面是对%d的特殊处理，如果%d后面直接跟了一对大括号，那么把大括号里面的内容提取出来作为dateformat
                    if (c != "d") {
                        i++;
                        continue;
                    }
                    i++;
                    if (i < m_pattern.size() && m_pattern[i] != '{') { continue; }
                    i++;
                    while (i < m_pattern.size() && m_pattern[i] != '}') {
                        dateformat.push_back(m_pattern[i]);
                        i++;
                    }
                    if (m_pattern[i] != '}') {
                        // %d后面的大括号没有闭合，直接报错
                        std::cout << "[ERROR] LogFormatter::init() " << "pattern: [" << m_pattern << "] '{' not closed"
                                  << std::endl;
                        error = true;
                        break;
                    }
                    i++;
                    continue;
                }
            }
        }

        if (error) {
            m_error = true;
            return;
        }

        // 模板解析结束之后剩余的常规字符也要算进去
        if (!tmp.empty()) {
            patterns.push_back(std::make_pair(0, tmp));
            tmp.clear();
        }

        static std::map<std::string, std::function<FormatItem::ptr(const std::string &str)>> s_format_items = {
#define XX(str, C) \
    { \
        #str, [](const std::string &fmt) { return FormatItem::ptr(new C(fmt)); } \
    }

            XX(m, MessageFormatItem), // m:消息
            XX(p, LevelFormatItem), // p:日志级别
            XX(c, LoggerNameFormatItem), // c:日志器名称
            //        XX(d, DateTimeFormatItem),          // d:日期时间
            XX(r, ElapseFormatItem), // r:累计毫秒数
            XX(f, FileNameFormatItem), // f:文件名
            XX(l, LineFormatItem), // l:行号
            XX(t, ThreadIdFormatItem), // t:编程号
            XX(F, FiberIdFormatItem), // F:协程号
            XX(N, ThreadNameFormatItem), // N:线程名称
            XX(%, PercentSignFormatItem), // %:百分号
            XX(T, TabFormatItem), // T:制表符
            XX(n, NewLineFormatItem), // n:换行符
#undef XX
        };

        for (auto &v : patterns) {
            if (v.first == 0) {
                m_items.push_back(FormatItem::ptr(new StringFormatItem(v.second)));
            } else if (v.second == "d") {
                m_items.push_back(FormatItem::ptr(new DateTimeFormatItem(dateformat)));
            } else {
                auto it = s_format_items.find(v.second);
                if (it == s_format_items.end()) {
                    std::cout << "[ERROR] LogFormatter::init() " << "pattern: [" << m_pattern << "] "
                              << "unknown format item: " << v.second << std::endl;
                    error = true;
                    break;
                } else {
                    m_items.push_back(it->second(v.second));
                }
            }
        }

        if (error) {
            m_error = true;
            return;
        }
    }


    std::string LogFormatter::format(LogEvent::ptr event)
    {
        std::stringstream ss;
        for (auto &i : m_items) {
            i->format(ss, event);
        }
        return ss.str();
    }

    std::ostream &LogFormatter::format(std::ostream &os, LogEvent::ptr event)
    {
        for (auto &i : m_items) {
            i->format(os, event);
        }
        return os;
    }

    LogAppender::LogAppender(LogFormatter::ptr default_formatter)
        : m_defaultFormatter(default_formatter)
    {}

    //默认使用m_defaultFormatter，只有当调用set函数的时候m_formatter才有值
    void LogAppender::setFormatter(LogFormatter::ptr val)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_formatter = val;
    }

    LogFormatter::ptr LogAppender::getFormatter()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_formatter ? m_formatter : m_defaultFormatter;
    }

    //先构造基类然后在构造自己
    StdoutLogAppender::StdoutLogAppender()
        : LogAppender(LogFormatter::ptr(new LogFormatter))
    {}

    void StdoutLogAppender::log(LogEvent::ptr event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_formatter) {
            m_formatter->format(std::cout, event);
        } else {
            m_defaultFormatter->format(std::cout, event);
        }
    }

    FileLogAppender::FileLogAppender(const std::string &file)
        : LogAppender(LogFormatter::ptr(new LogFormatter))
    {
        m_filename = file;
        reopen();
        if (m_reopenError) { std::cout << "reopen file " << m_filename << " error" << std::endl; }
    }

    void FileLogAppender::log(LogEvent::ptr event)
    {
        // m_lastTime / m_reopenError / m_filestream 都被多线程共享，整个函数持锁
        std::lock_guard<std::mutex> lock(m_mutex);

        uint64_t now = event->getTime();
        if (now >= (m_lastTime + 3)) {
            // 此时已持锁，内联reopen的逻辑（reopen内部也会加锁，不能直接调用）
            if (m_filestream) { m_filestream.close(); }
            m_filestream.open(m_filename, std::ios::app);
            m_reopenError = !m_filestream;
            if (m_reopenError) { std::cout << "reopen file " << m_filename << " error" << std::endl; }
            m_lastTime = now;
        }
        if (m_reopenError) { return; }

        if (m_formatter) {
            if (!m_formatter->format(m_filestream, event)) {
                std::cout << "[ERROR] FileLogAppender::log() format error" << std::endl;
            }
        } else {
            if (!m_defaultFormatter->format(m_filestream, event)) {
                std::cout << "[ERROR] FileLogAppender::log() format error" << std::endl;
            }
        }
    }

    bool FileLogAppender::reopen()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_filestream) { m_filestream.close(); }
        m_filestream.open(m_filename, std::ios::app);
        m_reopenError = !m_filestream;
        return !m_reopenError;
    }

    Logger::Logger(const std::string &name)
        : m_name(name)
        , m_level(LogLevel::INFO)
        , m_createTime(GetElapsedMS())
    {}

    void Logger::addAppender(LogAppender::ptr appender)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_appenders.push_back(appender);
    }

    void Logger::delAppender(LogAppender::ptr appender)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_appenders.begin(); it != m_appenders.end(); it++) {
            if (*it == appender) {
                m_appenders.erase(it);
                break;
            }
        }
    }

    void Logger::clearAppenders()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_appenders.clear();
    }

    void Logger::log(LogEvent::ptr event)
    {
        if (event->getLevel() >= m_level) {
            for (auto &i : m_appenders) {
                i->log(event);
            }
        }
    }

    LogEventWrap::LogEventWrap(Logger::ptr logger, LogEvent::ptr event)
        : m_logger(logger)
        , m_event(event)
    {}

    /**
    * 注意: LogEventWrap在析构时写日志
    */
    LogEventWrap::~LogEventWrap()
    {
        m_logger->log(m_event);
    }

    LoggerManager::LoggerManager()
    {
        // 支持容器化：MQ_HOST 环境变量（Docker 里指向 rabbitmq 服务名），默认本机
        const char* mq_host = std::getenv("MQ_HOST");
        m_root.reset(new Logger("root"));
        m_root->addAppender(LogAppender::ptr(new StdoutLogAppender));
        // root日志也发到RabbitMQ（只发MQ，不写本地文件，避免和system的AsyncLogAppender抢同一个文件）
        m_root->addAppender(LogAppender::ptr(
            new sylar::AsyncLogAppender("../../log.txt", 10000, 100, 100,
                                       true, mq_host ? mq_host : "127.0.0.1", 5672, "logs",
                                        false /*writeLocalFile*/)));
        m_loggers[m_root->getName()] = m_root;
        init();
    }

    Logger::ptr LoggerManager::getLogger(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_loggers.find(name);
        if(it != m_loggers.end()) {
            return it->second;
        }

        Logger::ptr logger = std::make_shared<Logger>(name);
        // 文件输出改为异步：日志只入队，由后台线程批量写盘，避免拖累工作线程
        sylar::LogAppender::ptr fileAppender(new sylar::AsyncLogAppender("../../log.txt"));
        logger->addAppender(fileAppender);
        logger->addAppender(LogAppender::ptr(new StdoutLogAppender));
        m_loggers[name] = logger;
        return logger;
    }

    void LoggerManager::init() {
    }

}
