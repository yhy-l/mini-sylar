/**
 * 日志模块用于输出系统信息
 *
 */

#pragma once
#include <string>
#include <memory>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <cstdarg>
#include <list>
#include <map>
#include <mutex>
#include <cstdint>
#include <atomic>
#include <format>

/**
 * 通过单例获取日志控制器
 */
#define SYLAR_LOG_ROOT() sylar::LoggerManager::getInstance().getRoot()

/**
 * 获取指定名称的日志器
 */
#define SYLAR_LOG_NAME(name) sylar::LoggerManager::getInstance().getLogger(name)

/**
 * 使用流式方式将日志级别level的日志写入到logger
 * 构造一个LogEventWrap对象，包裹包含日志器和日志事件，在对象析构时调用日志器写日志事件
 */
#define SYLAR_LOG_LEVEL(logger, level) \
    if (level >= logger->getLevel()) \
    sylar::LogEventWrap(logger, \
                        sylar::LogEvent::ptr(new sylar::LogEvent(logger->getName(), \
                                                                 level, \
                                                                 __FILE__, \
                                                                 __LINE__, \
                                                                 sylar::GetElapsedMS() - logger->getCreateTime(), \
                                                                 sylar::GetThreadId(), \
                                                                 sylar::GetFiberId(), \
                                                                 time(0), \
                                                                 sylar::GetThreadName()))) \
        .getLogEvent() \
        ->getSS()

#define SYLAR_LOG_FATAL(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::FATAL)

#define SYLAR_LOG_ALERT(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::ALERT)

#define SYLAR_LOG_CRIT(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::CRIT)

#define SYLAR_LOG_ERROR(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::ERROR)

#define SYLAR_LOG_WARN(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::WARN)

#define SYLAR_LOG_NOTICE(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::NOTICE)

#define SYLAR_LOG_INFO(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::INFO)

#define SYLAR_LOG_DEBUG(logger) SYLAR_LOG_LEVEL(logger, sylar::LogLevel::DEBUG)

namespace sylar {

    // 获取程序启动到现在的毫秒数
    inline uint64_t GetElapsedMS() {
        static auto start_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   now - start_time
                   ).count();
    }

    // 获取线程ID
    inline uint32_t GetThreadId() {
        static std::atomic<uint32_t> s_threadIdCounter{0};
        static thread_local uint32_t t_threadId = ++s_threadIdCounter;
        return t_threadId;
    }

    inline uint64_t GetFiberId() {
        return 0;
    }

    // 获取线程名字
    inline std::string GetThreadName() {
        return std::format("Thread_{}", GetThreadId());
    }

    /**
     * 日志级别
     */
    class LogLevel
    {
    public:

        /**
         * 日志级别枚举
         */
        enum Level {
            /// 致命情况，系统不可用
            FATAL = 800,
            /// 高优先级情况，例如数据库系统崩溃
            ALERT = 700,
            /// 严重错误，例如硬盘错误
            CRIT = 600,
            /// 错误
            ERROR = 500,
            /// 警告
            WARN = 400,
            /// 正常但值得注意
            NOTICE = 300,
            /// 一般信息
            INFO = 200,
            /// 调试信息
            DEBUG = 100,
            /// 未设置
            NOTSET = 0,
        };

        /**
        * 日志级别转字符串
        * 给一个level枚举，输出字符串level
        * level 日志级别
        * 返回: 字符串形式的日志级别
        */
        static const char *ToString(LogLevel::Level level);

        /**
        * 字符串转日志级别
        * 与tostring相反
        * str 字符串
        * 返回: 日志级别
        * 注意: 不区分大小写
        */
        static LogLevel::Level FromString(const std::string &str);
    };


    /**
     * 日志事件
     */
    class LogEvent
    {
    public:
        using ptr = std::shared_ptr<LogEvent>;

        /**
        * 构造函数
        * logger_name 日志器名称
        * level 日志级别
        * file 文件名
        * line 行号
        * elapse 从日志器创建开始到当前的累计运行毫秒
        * thead_id 线程id
        * fiber_id 协程id
        * time UTC时间
        * thread_name 线程名称
        */
        LogEvent(const std::string &logger_name,
                 LogLevel::Level level,
                 const char *file,
                 int32_t line,
                 int64_t elapse,
                 uint32_t thread_id,
                 uint64_t fiber_id,
                 time_t time,
                 const std::string &thread_name);


        /**
        * 获取日志级别
        */
        LogLevel::Level getLevel() const { return m_level; }

        /**
        * 获取日志内容
        */
        std::string getContent() const { return m_ss.str(); }

        /**
        * 获取文件名
        */
        std::string getFile() const { return m_file; }

        /**
        * 获取行号
        */
        int32_t getLine() const { return m_line; }

        /**
        * 获取累计运行毫秒数
        */
        int64_t getElapse() const { return m_elapse; }

        /**
        * 获取线程id
        */
        uint32_t getThreadId() const { return m_threadId; }

        /**
        * 获取协程id
        */
        uint64_t getFiberId() const { return m_fiberId; }

        /**
        * 返回时间戳
        */
        time_t getTime() const { return m_time; }

        /**
        * 获取线程名称
        */
        const std::string &getThreadName() const { return m_threadName; }

        /**
        * 获取内容字节流，用于流式写入日志
        */
        std::stringstream &getSS() { return m_ss; }

        /**
        * 获取日志器名称
        */
        const std::string &getLoggerName() const { return m_loggerName; }

    private:
        // 日志级别
        LogLevel::Level m_level;
        // 日志内容，使用stringstream存储，便于流式写入日志
        std::stringstream m_ss;
        // 文件名
        const char *m_file = nullptr;
        // 行号
        int32_t m_line = 0;
        // 从日志器创建开始到当前的耗时
        int64_t m_elapse = 0;
        // 线程id
        uint32_t m_threadId = 0;
        // 协程id
        uint64_t m_fiberId = 0;
        // UTC时间戳
        time_t m_time;
        // 线程名称
        std::string m_threadName;
        // 日志器名称
        std::string m_loggerName;
    };


    /**
    * 日志格式化
    */
    class LogFormatter
    {
    public:
        using ptr = std::shared_ptr<LogFormatter>;

        /**
        * 构造函数
        * pattern 格式模板，参考sylar与log4cpp
        * 模板参数说明：
        * - %%m 消息
        * - %%p 日志级别
        * - %%c 日志器名称
        * - %%d 日期时间，后面可跟一对括号指定时间格式，比如%%d{%%Y-%%m-%%d %%H:%%M:%%S}
        * - %%r 该日志器创建后的累计运行毫秒数
        * - %%f 文件名
        * - %%l 行号
        * - %%t 线程id
        * - %%F 协程id
        * - %%N 线程名称
        * - %%% 百分号
        * - %%T 制表符
        * - %%n 换行
        *
        * 默认格式：%%d{%%Y-%%m-%%d %%H:%%M:%%S}%%T%%t%%T%%N%%T%%F%%T[%%p]%%T[%%c]%%T%%f:%%l%%T%%m%%n
        *
        * 默认格式描述：年-月-日 时:分:秒 [累计运行毫秒数] \\t 线程id \\t 线程名称 \\t 协程id \\t [日志级别] \\t [日志器名称] \\t 文件名:行号 \\t 日志消息 换行符
        */
        LogFormatter(const std::string &pattern = "%d{%Y-%m-%d %H:%M:%S} [%rms]%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");

        /**
        * 初始化，解析格式模板，提取模板项
        */
        void init();

        /**
     * 模板解析是否出错
     */
        bool isError() const { return m_error; }

        /**
     * 对日志事件进行格式化，返回格式化日志文本
     * event 日志事件
     * 返回: 格式化日志字符串
     */
        std::string format(LogEvent::ptr event);

        /**
     * 对日志事件进行格式化，返回格式化日志流
     * event 日志事件
     * os 日志输出流
     * 返回: 格式化日志流
     */
        std::ostream &format(std::ostream &os, LogEvent::ptr event);

        /**
     * 获取pattern
     */
        std::string getPattern() const { return m_pattern; }

    public:
        /**
     * 日志内容格式化项，虚基类，用于派生出不同的格式化项
     */
        class FormatItem
        {
        public:
            using ptr = std::shared_ptr<FormatItem>;

            /**
         * 析构函数
         */
            virtual ~FormatItem() {}

            /**
         * 格式化日志事件
         */
            virtual void format(std::ostream &os, LogEvent::ptr event) = 0;
        };

    private:
        // 日志格式模板
        std::string m_pattern;
        // 解析后的格式模板数组
        std::vector<FormatItem::ptr> m_items;
        // 是否出错
        bool m_error = false;
    };


    /**
 * 日志输出地，虚基类，用于派生出不同的LogAppender
 * 参考log4cpp，Appender自带一个默认的LogFormatter，以控件默认输出格式
 */
    class LogAppender
    {
    public:
        using ptr = std::shared_ptr<LogAppender>;

        /**
     * 构造函数
     * default_formatter 默认日志格式器
     */
        LogAppender(LogFormatter::ptr default_formatter);

        /**
     * 析构函数
     */
        virtual ~LogAppender() {}

        /**
     * 设置日志格式器
     */
        void setFormatter(LogFormatter::ptr val);

        /**
     * 获取日志格式器
     */
        LogFormatter::ptr getFormatter();

        /**
     * 写入日志
     */
        virtual void log(LogEvent::ptr event) = 0;

    protected:
        std::mutex m_mutex;
        // 日志格式器
        LogFormatter::ptr m_formatter;
        // 默认日志格式器
        LogFormatter::ptr m_defaultFormatter;
    };

    /**
 * 输出到控制台的Appender
 */
    class StdoutLogAppender : public LogAppender
    {
    public:
        using ptr = std::shared_ptr<StdoutLogAppender>;

        /**
     * 构造函数
     */
        StdoutLogAppender();

        /**
     * 写入日志
     */
        void log(LogEvent::ptr event) override;
    };

    /**
 * 输出到文件
 */
    class FileLogAppender : public LogAppender
    {
    public:
        using ptr = std::shared_ptr<FileLogAppender>;

        /**
     * 构造函数
     * file 日志文件路径
     */
        FileLogAppender(const std::string &file);

        /**
     * 写日志
     */
        void log(LogEvent::ptr event) override;

        /**
     * 重新打开日志文件
     * 返回: 成功返回true
     */
        bool reopen();

    private:
        /// 文件路径
        std::string m_filename;
        /// 文件流
        std::ofstream m_filestream;
        /// 上次重打打开时间
        uint64_t m_lastTime = 0;
        /// 文件打开错误标识
        bool m_reopenError = false;
    };

    /**
 * 日志器类
 * 注意: 日志器类不带root logger
 */
    class Logger
    {
    public:
        using ptr = std::shared_ptr<Logger>;

        /**
     * 构造函数
     * name 日志器名称
     */
        Logger(const std::string &name = "default");

        /**
     * 获取日志器名称
     */
        const std::string &getName() const { return m_name; }

        /**
     * 获取创建时间
     */
        const uint64_t &getCreateTime() const { return m_createTime; }

        /**
     * 设置日志级别
     */
        void setLevel(LogLevel::Level level) { m_level = level; }

        /**
     * 获取日志级别
     */
        LogLevel::Level getLevel() const { return m_level; }

        /**
     * 添加LogAppender
     */
        void addAppender(LogAppender::ptr appender);

        /**
     * 删除LogAppender
     */
        void delAppender(LogAppender::ptr appender);

        /**
     * 清空LogAppender
     */
        void clearAppenders();

        /**
     * 写日志
     */
        void log(LogEvent::ptr event);

    private:
        std::mutex m_mutex;
        // 日志器名称
        std::string m_name;
        // 日志器等级
        LogLevel::Level m_level;
        // LogAppender集合
        std::list<LogAppender::ptr> m_appenders;
        // 创建时间（毫秒）
        uint64_t m_createTime;
    };

    /**
 * 日志事件包装器，方便宏定义，内部包含日志事件和日志器
 */
    class LogEventWrap
    {
    public:
        /**
     * 构造函数
     * logger 日志器
     * event 日志事件
     */
        LogEventWrap(Logger::ptr logger, LogEvent::ptr event);

        /**
     * 析构函数
     * 日志事件在析构时由日志器进行输出
     */
        ~LogEventWrap();

        /**
     * 获取日志事件
     */
        LogEvent::ptr getLogEvent() const { return m_event; }

    private:
        // 日志器
        Logger::ptr m_logger;
        // 日志事件
        LogEvent::ptr m_event;
    };

    /**
 * 日志器管理类
 */
    class LoggerManager
    {
    public:
        /**
         * 单例实现
         */
        static LoggerManager &getInstance()
        {
            static LoggerManager instance;
            return instance;
        }
        // 删除拷贝构造和赋值
        LoggerManager(const LoggerManager&) = delete;
        LoggerManager& operator=(const LoggerManager&) = delete;


        /**
     * 初始化，主要是结合配置模块实现日志模块初始化
     */
        void init();

        /**
     * 获取指定名称的日志器
     */
        Logger::ptr getLogger(const std::string &name);

        /**
     * 获取root日志器，等效于getLogger("root")
     */
        Logger::ptr getRoot() { return m_root; }

    private:
    /**
     * 构造函数
     */
        LoggerManager();

        std::mutex m_mutex;
        // 日志器集合
        std::map<std::string, Logger::ptr> m_loggers;
        // root日志器
        Logger::ptr m_root;
    };

    /**
     * @brief 获取缓存的 system 日志器
     *
     * SYLAR_LOG_NAME("system") 每次调用都要在 LoggerManager 的全局锁下查表，
     * 放在"每请求都要执行"的热路径上会变成多线程锁竞争点。
     * 这里用函数内静态变量缓存一次，之后只是返回已有智能指针的引用。
     */
    inline const Logger::ptr& SystemLogger()
    {
        static Logger::ptr s_logger = LoggerManager::getInstance().getLogger("system");
        return s_logger;
    }

}
