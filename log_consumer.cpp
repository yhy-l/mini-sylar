#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <amqp_framing.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <ctime>
#include <filesystem>
#include <cstdio>
#include <csignal>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

/// 统计信息（每条消息处理时更新）
struct LogStats {
    long long total = 0;
    std::map<std::string, long long> levelCount;
    std::vector<time_t> errorTimes;   // 近60秒的错误时间戳，用于告警
};

/**
 * 处理一条MQ消息（可包含多行JSON）
 * 解析每条日志：按级别分文件、错误统计告警、结构化落盘、更新统计
 */
static void process_message(const std::string& body,
                            std::ofstream& infoLog,
                            std::ofstream& errorLog,
                            std::ofstream& structLog,
                            std::ofstream& alertLog,
                            LogStats& stats)
{
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) { nl = body.size(); }
        std::string line = body.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) { continue; }

        try {
            nlohmann::json j = nlohmann::json::parse(line);
            std::string level = j.value("level", "INFO");
            bool isError = (level == "ERROR" || level == "CRIT"
                            || level == "ALERT" || level == "FATAL");

            ++stats.total;
            ++stats.levelCount[level];

            if (isError) {
                errorLog << line << "\n";
                errorLog.flush();

                time_t now = time(nullptr);
                stats.errorTimes.push_back(now);
                while (!stats.errorTimes.empty()
                       && now - stats.errorTimes.front() > 60) {
                    stats.errorTimes.erase(stats.errorTimes.begin());
                }
                if (stats.errorTimes.size() >= 10) {
                    std::string alert = "[ALERT] 60秒内错误日志达到 "
                                        + std::to_string(stats.errorTimes.size()) + " 条\n";
                    fprintf(stdout, "%s", alert.c_str());
                    alertLog << alert;
                    alertLog.flush();
                    stats.errorTimes.clear();
                }
            } else {
                infoLog << line << "\n";
                infoLog.flush();
            }

            structLog << line << "\n";
            structLog.flush();
        } catch (...) {
            fprintf(stderr, "[log_consumer] JSON解析失败: %s\n", line.c_str());
        }
    }
}

static bool check_reply(amqp_connection_state_t conn, const char* what)
{
    amqp_rpc_reply_t reply = amqp_get_rpc_reply(conn);
    if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
        if (reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION) {
            fprintf(stderr, "[log_consumer] %s: %s\n", what, amqp_error_string2(reply.library_error));
        } else if (reply.reply_type == AMQP_RESPONSE_SERVER_EXCEPTION
                   && reply.reply.id == AMQP_CHANNEL_CLOSE_METHOD) {
            amqp_channel_close_t* m = static_cast<amqp_channel_close_t*>(reply.reply.decoded);
            fprintf(stderr, "[log_consumer] %s: channel_close code=%u text=%.*s\n",
                    what, m->reply_code, (int)m->reply_text.len, (char*)m->reply_text.bytes);
        } else {
            fprintf(stderr, "[log_consumer] %s failed (reply_type=%d id=%d)\n", what, reply.reply_type, reply.reply.id);
        }
        return false;
    }
    return true;
}

int main()
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // 输出目录
    std::filesystem::create_directories("logs");
    std::ofstream infoLog("logs/info.log", std::ios::app);
    std::ofstream errorLog("logs/error.log", std::ios::app);
    std::ofstream structLog("logs/structured.log", std::ios::app);
    std::ofstream alertLog("logs/alert.log", std::ios::app);

    amqp_connection_state_t conn = amqp_new_connection();
    amqp_socket_t* socket = amqp_tcp_socket_new(conn);
    if (!socket || amqp_socket_open(socket, "127.0.0.1", 5672) != AMQP_STATUS_OK) {
        fprintf(stderr, "[log_consumer] 无法连接RabbitMQ 127.0.0.1:5672\n");
        return 1;
    }
    amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, "guest", "guest");
    amqp_channel_open(conn, 1);
    if (!check_reply(conn, "channel_open")) { return 1; }
    amqp_queue_declare(conn, 1, amqp_cstring_bytes("logs"),
                       0 /*passive*/, 1 /*durable*/, 0, 0, amqp_empty_table);
    if (!check_reply(conn, "queue_declare")) { return 1; }
    amqp_basic_consume(conn, 1, amqp_cstring_bytes("logs"),
                       amqp_empty_bytes, 0, 0 /*no_ack=false*/, 0, amqp_empty_table);
    if (!check_reply(conn, "basic_consume")) { return 1; }

    fprintf(stdout, "[log_consumer] 开始消费队列 logs（Ctrl+C退出）\n");

    LogStats logStats;

    auto writeStats = [&]() {
        std::ofstream statsFile("logs/stats.log", std::ios::trunc);
        char tbuf[32];
        time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);

        statsFile << "===== 日志统计 =====" << "\n";
        statsFile << "更新时间: " << tbuf << "\n";
        statsFile << "总消息数: " << logStats.total << "\n";
        statsFile << "级别分布:\n";
        for (const char* lv : {"DEBUG", "INFO", "NOTICE", "WARN",
                               "ERROR", "CRIT", "ALERT", "FATAL"}) {
            statsFile << "  " << lv << " : " << logStats.levelCount[lv] << "\n";
        }
        long long errTotal = logStats.levelCount["ERROR"] + logStats.levelCount["CRIT"]
                           + logStats.levelCount["ALERT"] + logStats.levelCount["FATAL"];
        statsFile << "错误占比: " << errTotal << "/" << logStats.total
                  << " (" << (logStats.total ? errTotal * 100 / logStats.total : 0) << "%)\n";
        statsFile.flush();
    };

    while (!g_stop) {
        amqp_envelope_t envelope;
        amqp_maybe_release_buffers(conn);
        struct timeval tv = { 1, 0 };   // 1秒超时，空闲时定期检查停止标志
        amqp_rpc_reply_t res = amqp_consume_message(conn, &envelope, &tv, 0);
        if (res.reply_type != AMQP_RESPONSE_NORMAL) {
            if (g_stop) { break; }
            if (res.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION
                && res.library_error == AMQP_STATUS_TIMEOUT) {
                continue;   // 空闲超时（无消息），回去检查停止标志
            }
            fprintf(stderr, "[log_consumer] 消费错误 (reply_type=%d)\n", res.reply_type);
            break;
        }

        std::string body(static_cast<char*>(envelope.message.body.bytes),
                         envelope.message.body.len);

        // 取出消息 → 交给处理函数
        process_message(body, infoLog, errorLog, structLog, alertLog, logStats);

        if (logStats.total % 50 == 0) {
            writeStats();   // 每50条刷新一次统计文件
        }

        amqp_basic_ack(conn, 1, envelope.delivery_tag, 0);
        amqp_destroy_envelope(&envelope);
    }

    writeStats();   // 退出前写最终统计
    fprintf(stdout, "[log_consumer] 退出\n");
    amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
    amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(conn);
    return 0;
}
