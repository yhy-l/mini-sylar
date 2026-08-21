#pragma once

#include "http_parser.h"
#include "http_request.h"
#include "../tcpserver.h"
#include <functional>
#include <unordered_map>
#include <string>
#include <memory>
#include <cstring>
#include <ctime>
#include "../log.h"
#include "../task.h"
#include "../getFileDate.h"
#include "../redis_client.h"

namespace sylar {

    extern sylar::Logger::ptr g_logger;

    using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

    class HttpServer : public TcpServer
    {
    private:
        std::unordered_map<std::string, HttpHandler> routes_;

        /// 静态文件目录（页面/样式/脚本从这里读取，改页面不用重新编译）
        std::string m_staticDir = "web";

        /// Redis 客户端（可空：未配置时限流/统计自动禁用）
        RedisClient::ptr m_redis;

        /// 接口限流规则："METHOD:path" -> {窗口内上限, 窗口秒数}
        std::unordered_map<std::string, std::pair<int, int>> m_rateLimits;

        /**
         * 非阻塞发送：循环发送直到全部发出或遇到 EAGAIN（缓冲满）
         * 返回: 1=全部发完, 0=EAGAIN（调用方需等可写后重试）, -1=错误
         */
        static int send_nonblocking(Socket::ptr client, const std::string& data, size_t& off)
        {
            while (off < data.size()) {
                ssize_t n = client->send(data.data() + off, data.size() - off);
                if (n > 0) {
                    off += n;
                    continue;
                }
                if (n == 0) return 0;   // 非阻塞 EAGAIN：发送缓冲区满
                return -1;
            }
            return 1;
        }

        /**
         * 把 "k1=v1,k2=v2" 形式的文本转成 JSON 对象
         */
        static std::string kv_to_json(const std::string& s)
        {
            std::string out = "{";
            bool first = true;
            size_t pos = 0;
            while (pos < s.size()) {
                size_t comma = s.find(',', pos);
                std::string pair = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                pos = (comma == std::string::npos) ? s.size() : comma + 1;
                if (pair.empty()) continue;
                size_t eq = pair.find('=');
                std::string k = pair.substr(0, eq);
                std::string v = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
                if (!first) out += ",";
                first = false;
                out += "\"" + k + "\":" + (v.empty() ? "0" : v);
            }
            out += "}";
            return out;
        }

        /**
         * 根据文件扩展名返回Content-Type
         */
        static std::string content_type_by_ext(const std::string& path)
        {
            if (path.ends_with(".html")) return "text/html; charset=utf-8";
            if (path.ends_with(".css"))  return "text/css; charset=utf-8";
            if (path.ends_with(".js"))   return "application/javascript; charset=utf-8";
            if (path.ends_with(".json")) return "application/json; charset=utf-8";
            if (path.ends_with(".png"))  return "image/png";
            if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
            if (path.ends_with(".svg"))  return "image/svg+xml";
            if (path.ends_with(".ico"))  return "image/x-icon";
            return "application/octet-stream";
        }

        /**
         * 尝试从静态目录读取文件并构造响应
         * URL路径 → 静态目录下的文件："/"→index.html，"/css/a.css"→web/css/a.css
         */
        HttpResponse try_static_file(const std::string& path)
        {
            std::string rel = path;
            if (rel == "/") { rel = "/index.html"; }

            // 防目录穿越：拦截包含".."的路径
            if (rel.find("..") != std::string::npos) {
                return HttpResponse::make_error_response(403, "Forbidden");
            }

            std::string file = m_staticDir + rel;

            // 先确认文件存在，避免read_binary_file每次打错误日志
            if (!std::ifstream(file, std::ios::binary).good()) {
                return HttpResponse::make_error_response(404, "Not Found");
            }

            std::vector<unsigned char> data = read_binary_file(file);
            if (data.empty()) {
                return HttpResponse::make_error_response(404, "Not Found");
            }

            HttpResponse res;
            res.set_content_type(content_type_by_ext(rel));
            res.body.assign(data.begin(), data.end());
            res.auto_set_content_length();
            return res;
        }

        //默认处理：返回404
        HttpResponse default_handler(const HttpRequest& req)
        {
            return HttpResponse::make_error_response(404,
                                                     "The requested URL " + req.path + " was not found on this server.");
        }

    public:

        HttpServer(sylar::IOManager* io_worker = sylar::IOManager::GetThis(),
                   sylar::IOManager* accept_worker = sylar::IOManager::GetThis())
            : TcpServer(io_worker, accept_worker) {

            if (m_ioWorker == nullptr)
                SYLAR_LOG_ERROR(g_logger) << "ioWorker指针为空！";
            else
                SYLAR_LOG_INFO(g_logger) << "io指针为" << m_ioWorker;

            if (m_acceptWorker == nullptr)
                SYLAR_LOG_ERROR(g_logger) << "accept指针为空!";
            else
                SYLAR_LOG_INFO(g_logger) << "work指针为："<<m_acceptWorker;

        }

        /**
         * 设置静态文件目录（存放html/css/js等页面资源）
         */
        void setStaticDir(const std::string& dir) { m_staticDir = dir; }

        /**
         * 挂载 Redis 客户端（限流 + 实时统计）
         */
        void setRedisClient(RedisClient::ptr redis) { m_redis = std::move(redis); }

        /**
         * 配置接口限流：每 IP 在 windowSec 秒内最多访问 limit 次，超限返回 429
         */
        void setRateLimit(const std::string& path, int limit, int windowSec,
                          const std::string& method = "GET")
        {
            m_rateLimits[method + ":" + path] = {limit, windowSec};
        }

        void get(const std::string& path, HttpHandler handler) { routes_["GET:" + path] = handler; }

        void post(const std::string& path, HttpHandler handler) { routes_["POST:" + path] = handler; }

        void put(const std::string& path, HttpHandler handler) { routes_["PUT:" + path] = handler; }

        void del(const std::string& path, HttpHandler handler) { routes_["DELETE:" + path] = handler; }

        HttpResponse handle_request(const HttpRequest& req)
        {
            std::string key = req.method + ":" + req.path;
            auto it = routes_.find(key);

            if (it != routes_.end()) { return it->second(req); }

            // 路由未命中时，尝试静态文件（页面/样式/脚本）
            if (req.method == "GET") {
                HttpResponse res = try_static_file(req.path);
                if (res.status_code != 404) { return res; }
            }

            return default_handler(req);
        }

        // 注册一些示例路由
        void register_example_routes()
        {
            // "/" 和 "/about.html" 由静态文件目录提供（web/index.html、web/about.html）

            /// JSON API示例
            get("/api/hello", [](const HttpRequest& req) {
                std::string json = R"({
    "status": "success",
    "message": "Hello from HTTP Server!",
    "timestamp": )" + std::to_string(time(nullptr))
                                   + R"(
})";
                return HttpResponse::make_json_response(json);
            });

            /// 返回当前时间的API
            get("/api/time", [](const HttpRequest& req) {
                time_t now = time(nullptr);
                // localtime()不是线程安全的（内部有静态状态），并发请求会数据竞争，用localtime_r
                struct tm timeinfo;
                localtime_r(&now, &timeinfo);
                char buffer[80];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);

                std::string json = R"({
    "timestamp": )" + std::to_string(now)
                                   + R"(,
    "datetime": ")" + std::string(buffer)
                                   + R"("
})";
                return HttpResponse::make_json_response(json);
            });

            /// POST示例
            post("/api/echo", [](const HttpRequest& req) {
                // 简单的echo接口，返回客户端发送的数据
                std::string json = R"({
    "method": ")" + req.method + R"(",
    "path": ")" + req.path + R"(",
    "content_length": ")" + std::to_string(req.body.size())
                                   + R"(",
    "body": ")" + req.body + R"("
})";
                return HttpResponse::make_json_response(json);
            });

            /// 返回静态文本
            get("/text", [](const HttpRequest& req) {
                std::string text = R"(This is a plain text response.

You can see that the Content-Type is text/plain.

Line 1
Line 2
Line 3)";
                return HttpResponse::make_text_response(text);
            });
        }

        sylar::Task handleClient(std::shared_ptr<TcpServer> self, Socket::ptr client) override
        {
            (void) self; // 仅用于在协程帧中持有服务器，保证服务器存活
            SYLAR_LOG_INFO(g_logger) << "====== HttpServer::handleClient 被调用 ======";
            SYLAR_LOG_INFO(g_logger) << "客户端: " << client->toString();

            // 在线连接数 +1（fire-and-forget，不等结果）
            if (m_redis) {
                m_redis->countAsync("INCR stats:conns");
            }

            char buffer[4096];
            std::string read_buffer; // 累积缓冲：一个请求可能分多次到达，也可能一次到达多个

            while (true) {
                // 第一步：先看累积缓冲里是否已经有一条完整请求
                // 完整 = 找到头部结束符 "\r\n\r\n"，并且 body 收满 Content-Length 字节
                size_t header_end = read_buffer.find("\r\n\r\n");
                size_t total_len = std::string::npos;
                if (header_end != std::string::npos) {
                    size_t body_len = 0;
                    HttpParser::find_content_length(read_buffer.substr(0, header_end), body_len);
                    total_len = header_end + 4 + body_len;
                }

                if (header_end == std::string::npos || read_buffer.size() < total_len) {
                    // 缓冲里的数据不足一个完整请求，等待更多数据
                    bool ready = co_await m_ioWorker->waitReadAsync(client->getSocket(), 5000);

                    if (!ready || m_isStop) {
                        SYLAR_LOG_INFO(g_logger) << "等待读取超时或服务器停止";
                        break;
                    }

                    // 读取数据
                    ssize_t n = client->recv(buffer, sizeof(buffer) - 1);
                    if (n <= 0) {
                        if (n == 0) {
                            SYLAR_LOG_INFO(g_logger) << "客户端正常关闭连接";
                        } else {
                            SYLAR_LOG_ERROR(g_logger) << "recv错误: " << strerror(errno);
                        }
                        break;
                    }

                    read_buffer.append(buffer, n);
                    SYLAR_LOG_INFO(g_logger) << "收到数据，本次: " << n
                                             << " 字节，累积: " << read_buffer.size() << " 字节";
                    continue;
                }

                // 第二步：缓冲里已有一条完整请求，取出来处理
                std::string request_str = read_buffer.substr(0, total_len);
                read_buffer.erase(0, total_len);

                SYLAR_LOG_INFO(g_logger) << "收到完整HTTP请求，长度: " << total_len;
                SYLAR_LOG_DEBUG(g_logger) << "请求内容: " << request_str;

                // 解析请求
                HttpRequest req;
                if (!HttpParser::parse_request(request_str, req)) {
                    SYLAR_LOG_ERROR(g_logger) << "解析HTTP请求失败";

                    // 发送400错误响应：先直接发，缓冲满才挂起等可写
                    HttpResponse bad_req = HttpResponse::make_error_response(400, "Bad Request");
                    std::string response_str = bad_req.to_string();

                    size_t sent_off = 0;
                    int sret = send_nonblocking(client, response_str, sent_off);
                    while (sret == 0) {
                        bool write_ready = co_await m_ioWorker->waitWriteAsync(client->getSocket(), 5000);
                        if (!write_ready) {
                            SYLAR_LOG_WARN(g_logger) << "等待写入超时";
                            break;
                        }
                        sret = send_nonblocking(client, response_str, sent_off);
                    }
                    if (sret == -1) {
                        SYLAR_LOG_ERROR(g_logger) << "发送400错误响应失败: " << strerror(errno);
                    } else {
                        SYLAR_LOG_INFO(g_logger) << "发送400错误响应成功，发送字节数: " << sent_off;
                    }
                    break;
                }

                SYLAR_LOG_INFO(g_logger) << "解析请求成功: " << req.method << " " << req.path;

                // —— Redis 限流 / 实时统计 ——
                HttpResponse res;
                bool responded = false;

                if (m_redis && req.path == "/api/stats") {
                    // 实时统计：需要 Redis 结果，特判并 co_await
                    auto r = co_await m_redis->command({
                        "GET stats:total",
                        "GET stats:conns",
                        "HGETALL stats:status",
                        "HGETALL stats:api",
                    });
                    std::string json;
                    if (r[0] == "-1") {
                        // Redis 不可用：明确降级提示
                        json = R"({"error":"redis unavailable"})";
                    } else {
                        json = "{";
                        json += "\"total\":" + (r[0].empty() ? "0" : r[0]) + ",";
                        json += "\"conns\":" + (r[1].empty() ? "0" : r[1]) + ",";
                        json += "\"status\":" + kv_to_json(r[2]) + ",";
                        json += "\"api\":" + kv_to_json(r[3]);
                        json += "}";
                    }
                    res = HttpResponse::make_json_response(json);
                    responded = true;
                } else if (m_redis) {
                    // 接口限流：INCR + EXPIRE（每次刷新TTL，等价"最近N秒最多limit次"）
                    std::string rkey = req.method + ":" + req.path;
                    auto it = m_rateLimits.find(rkey);
                    if (it != m_rateLimits.end()) {
                        std::string ip = "unknown";
                        if (client->getRemoteAddress()) {
                            std::string peer = client->getRemoteAddress()->toString();
                            auto pos = peer.find(':');
                            ip = (pos == std::string::npos) ? peer : peer.substr(0, pos);
                        }
                        std::string key = "rate:" + ip + ":" + rkey;
                        auto r = co_await m_redis->command({
                            "INCR " + key,
                            "EXPIRE " + key + " " + std::to_string(it->second.second),
                        });
                        long long cnt = 0;
                        if (!r[0].empty()) cnt = std::stoll(r[0]);
                        if (cnt > it->second.first) {
                            SYLAR_LOG_WARN(g_logger) << "触发限流: " << rkey
                                                     << " ip=" << ip << " count=" << cnt;
                            res = HttpResponse::make_error_response(429, "Too Many Requests");
                            responded = true;
                        }
                    }
                }

                // 非特判路径走正常路由
                if (!responded) {
                    res = handle_request(req);
                }

                // —— 统计上报（fire-and-forget，不阻塞）——
                if (m_redis) {
                    m_redis->countAsync("INCR stats:total");
                    m_redis->countAsync("HINCRBY stats:api " + req.method + ":" + req.path + " 1");
                    m_redis->countAsync("HINCRBY stats:status " + std::to_string(res.status_code) + " 1");
                }

                if (req.is_keep_alive()) {
                    res.headers["Connection"] = "keep-alive";
                } else {
                    res.headers["Connection"] = "close";
                }

                std::string response_str = res.to_string();
                SYLAR_LOG_DEBUG(g_logger) << "响应内容: " << response_str;

                // 发送响应：先直接发，缓冲满才挂起等可写（写路径免挂起）
                size_t sent_off = 0;
                int sret = send_nonblocking(client, response_str, sent_off);
                while (sret == 0) {
                    bool write_ready = co_await m_ioWorker->waitWriteAsync(client->getSocket(), 5000);
                    if (!write_ready) {
                        SYLAR_LOG_WARN(g_logger) << "等待写入超时";
                        break;
                    }
                    sret = send_nonblocking(client, response_str, sent_off);
                }
                if (sret == -1) {
                    SYLAR_LOG_ERROR(g_logger) << "send错误: " << strerror(errno);
                    break;
                }
                SYLAR_LOG_INFO(g_logger) << "发送响应成功，发送字节数: " << sent_off;

                // 如果是短连接，则退出循环
                if (!req.is_keep_alive()) {
                    SYLAR_LOG_INFO(g_logger) << "短连接，关闭连接";
                    break;
                }

                SYLAR_LOG_DEBUG(g_logger) << "长连接，回到循环检查下一个请求";
            }

            client->close();
            if (m_redis) {
                m_redis->countAsync("DECR stats:conns");   // 在线连接数 -1
            }
            removeClient(client);
            SYLAR_LOG_INFO(g_logger) << "连接已关闭";
            co_return;
        }

    };

}
