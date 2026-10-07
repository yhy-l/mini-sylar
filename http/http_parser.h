#pragma once

#include "http_request.h"
#include <string>
#include <string_view>
#include <algorithm>
#include <cctype>
#include <vector>

namespace sylar {
    extern sylar::Logger::ptr g_logger;

    class HttpParser
    {
    public:

        /**
     * 解析http请求
     * 使用 string_view 零拷贝解析：内部切分不分配内存，只在写入 request 字段时构造 string
     */
        static bool parse_request(std::string_view raw_request, HttpRequest& request)
        {
            if (raw_request.empty()) return false;

            // 头部结束位置："\r\n\r\n" 之前是请求行 + 所有头部
            size_t header_end = raw_request.find("\r\n\r\n");
            if (header_end == std::string_view::npos) {
                SYLAR_LOG_ERROR(g_logger) << "请求头部不完整";
                return false;
            }

            // 解析请求行（第一行）
            size_t line_end = raw_request.find("\r\n");
            if (line_end == std::string_view::npos || line_end > header_end) {
                SYLAR_LOG_ERROR(g_logger) << "解析头行失败";
                return false;
            }
            if (!parse_request_line(raw_request.substr(0, line_end), request)) {
                SYLAR_LOG_ERROR(g_logger) << "解析头行失败";
                return false;
            }

            // 解析请求行之后的头部，直到空行
            size_t pos = line_end + 2;
            while (pos < header_end) {
                size_t eol = raw_request.find("\r\n", pos);
                if (eol == std::string_view::npos || eol > header_end) {
                    eol = header_end;
                }
                if (!parse_header_line(raw_request.substr(pos, eol - pos), request)) {
                    SYLAR_LOG_ERROR(g_logger) << "解析头部失败";
                    return false;
                }
                pos = eol + 2;
            }

            // 请求体按 Content-Length 精确截取
            size_t body_len = 0;
            find_content_length(raw_request.substr(0, header_end), body_len);
            size_t available = raw_request.size() - (header_end + 4);
            if (available < body_len) {
                SYLAR_LOG_ERROR(g_logger) << "请求体不完整";
                return false;
            }
            request.body.assign(raw_request.data() + header_end + 4, body_len);
            return true;
        }

        /**
     * 从头部块中解析 Content-Length
     * headers_block: 请求行 + 头部文本（不含结尾空行）
     * length: 解析出的请求体长度
     */
        static bool find_content_length(std::string_view headers_block, size_t& length)
        {
            size_t pos = 0;
            while (pos <= headers_block.size()) {
                size_t eol = headers_block.find("\r\n", pos);
                std::string_view line = (eol == std::string_view::npos)
                                            ? headers_block.substr(pos)
                                            : headers_block.substr(pos, eol - pos);
                if (line.empty()) { break; }

                size_t colon = line.find(':');
                if (colon != std::string_view::npos) {
                    std::string_view key = trim_view(line.substr(0, colon));
                    if (key.size() == 14 && case_insensitive_equal(key, "content-length")) {
                        try {
                            length = std::stoul(std::string(trim_view(line.substr(colon + 1))));
                            return true;
                        } catch (...) {
                            return false;
                        }
                    }
                }

                if (eol == std::string_view::npos) { break; }
                pos = eol + 2;
            }
            return false;
        }

    private:
        /**
     * 解析请求行，如 "GET /index.html HTTP/1.1"
     * 手写切分，避免 split 产生 vector<string> 的多次分配
     */
        static bool parse_request_line(std::string_view line, HttpRequest& request)
        {
            size_t first_space = line.find(' ');
            if (first_space == std::string_view::npos) {
                SYLAR_LOG_ERROR(g_logger) << "头行解析错误";
                return false;
            }
            size_t second_space = line.find(' ', first_space + 1);
            if (second_space == std::string_view::npos) {
                SYLAR_LOG_ERROR(g_logger) << "头行解析错误";
                return false;
            }

            std::string_view method = line.substr(0, first_space);
            std::string_view path = line.substr(first_space + 1, second_space - first_space - 1);
            std::string_view version = line.substr(second_space + 1);
            if (method.empty() || path.empty() || version.empty()) {
                SYLAR_LOG_ERROR(g_logger) << "头行解析错误";
                return false;
            }

            request.method.assign(method.data(), method.size());

            // 查询参数暂不解析，只保留路径部分
            size_t query_pos = path.find('?');
            if (query_pos != std::string_view::npos) {
                path = path.substr(0, query_pos);
            }
            request.path.assign(path.data(), path.size());
            request.version.assign(version.data(), version.size());
            return true;
        }

        /**
     * 解析头部行，如 "Host: localhost:8080"
     */
        static bool parse_header_line(std::string_view line, HttpRequest& request)
        {
            size_t colon_pos = line.find(':');
            if (colon_pos == std::string_view::npos) {
                SYLAR_LOG_ERROR(g_logger) << "解析头部错误";
                return false;
            }

            std::string key(trim_view(line.substr(0, colon_pos)));
            to_lower_inplace(key);
            request.headers[std::move(key)] = std::string(trim_view(line.substr(colon_pos + 1)));
            return true;
        }

    private:
        /// 去掉两端的空白（view 版，零拷贝）
        static std::string_view trim_view(std::string_view s)
        {
            size_t begin = 0;
            size_t end = s.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) { ++begin; }
            while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) { --end; }
            return s.substr(begin, end - begin);
        }

        /// 原地转小写，避免额外分配
        static void to_lower_inplace(std::string& str)
        {
            std::transform(str.begin(), str.end(), str.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }

        /// 大小写不敏感比较（用于 Content-Length 等头部名）
        static bool case_insensitive_equal(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) { return false; }
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        }

        /**
     * 去除字符串两边的空白
     */
        static std::string trim(const std::string& str) {
            auto start = str.begin();
            auto end = str.end();

            while (start != end && std::isspace(static_cast<unsigned char>(*start))) {
                ++start;
            }

            do {
                --end;
            } while (std::distance(start, end) > 0 && std::isspace(static_cast<unsigned char>(*end)));

            return std::string(start, end + 1);
        }

        /**
     * 将字符串转换成小写
     */
        static std::string to_lower(const std::string& str) {
            std::string result = str;
            to_lower_inplace(result);
            return result;
        }

        /**
     * 按分隔符切分字符串
     */
        static std::vector<std::string> split(const std::string& str,
                                              const std::string& delimiter) {
            std::vector<std::string> tokens;
            size_t start = 0;
            size_t end = str.find(delimiter);

            while (end != std::string::npos) {
                tokens.push_back(str.substr(start, end - start));
                start = end + delimiter.length();
                end = str.find(delimiter, start);
            }

            tokens.push_back(str.substr(start));
            return tokens;
        }

    };

}
