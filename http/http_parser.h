#pragma once

#include "http_request.h"
#include <string>
#include <algorithm>
#include <cctype>

namespace sylar {
    extern sylar::Logger::ptr g_logger;

    class HttpParser
    {
    public:

        /**
     * 解析http
     * 整个http的内容:
     * http存储:
     * 
     */
        static bool parse_request(const std::string& raw_request, HttpRequest& request)
        {
            SYLAR_LOG_DEBUG(g_logger) << "开始解析HTTP请求";
            SYLAR_LOG_DEBUG(g_logger) << "请求字符串长度: " << raw_request.length();
            SYLAR_LOG_DEBUG(g_logger) << "请求内容:\n" << raw_request;


            if (raw_request.empty()) return false;

            // 头部结束位置："\r\n\r\n" 之前是请求行 + 所有头部
            size_t header_end = raw_request.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                SYLAR_LOG_ERROR(g_logger) << "请求头部不完整";
                return false;
            }

            // 解析请求行（第一行）
            size_t line_end = raw_request.find("\r\n");
            if (line_end == std::string::npos || line_end > header_end) {
                SYLAR_LOG_ERROR(g_logger) << "解析头行失败";
                return false;
            }
            if (!parse_request_line(raw_request.substr(0, line_end), request)) {
                SYLAR_LOG_ERROR(g_logger) << "解析头行失败";
                return false;
            }

            // 解析每一个头部（从请求行下一行开始，到空行结束）
            size_t pos = line_end + 2;
            while (pos < header_end) {
                size_t eol = raw_request.find("\r\n", pos);
                if (eol == std::string::npos || eol > header_end) {
                    eol = header_end;
                }
                std::string line = raw_request.substr(pos, eol - pos);
                if (line.empty()) { break; }

                if (!parse_header_line(line, request)) {
                    SYLAR_LOG_ERROR(g_logger) << "解析头部失败";
                    return false;
                }
                pos = eol + 2;
            }

            // 请求体按 Content-Length 精确截取，不再把"空行后的所有行"拼起来当body
            size_t body_len = 0;
            find_content_length(raw_request.substr(0, header_end), body_len);
            size_t available = raw_request.size() - (header_end + 4);
            if (available < body_len) {
                SYLAR_LOG_ERROR(g_logger) << "请求体不完整";
                return false;
            }
            request.body = raw_request.substr(header_end + 4, body_len);

            return true;

        }

        /**
         * 从头部块中解析 Content-Length
         * headers_block 请求行+头部的文本（不含结尾空行）
         * length 解析出的请求体长度
         * 返回: 找到返回true，没找到返回false
         */
        static bool find_content_length(const std::string& headers_block, size_t& length)
        {
            size_t pos = 0;
            while (pos <= headers_block.size()) {
                size_t eol = headers_block.find("\r\n", pos);
                std::string line = (eol == std::string::npos)
                                       ? headers_block.substr(pos)
                                       : headers_block.substr(pos, eol - pos);

                if (line.empty()) { break; }

                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string key = to_lower(trim(line.substr(0, colon)));
                    if (key == "content-length") {
                        try {
                            length = std::stoul(trim(line.substr(colon + 1)));
                            return true;
                        } catch (...) {
                            return false;
                        }
                    }
                }

                if (eol == std::string::npos) { break; }
                pos = eol + 2;
            }
            return false;
        }

    private:
        /**
     * 解析请求行（头行），如: "GET /index.html HTTP/1.1"
     * 头行string:
     * 结果位置request:
     * 
     */
        static bool parse_request_line(const std::string& line, HttpRequest& request)
        {
            std::vector<std::string> parts = split(line, " ");
            if (parts.size() != 3) {
                SYLAR_LOG_ERROR(g_logger) << "头行解析错误";
                return false;
            }

            request.method = parts[0]; //GET,POST等
            request.path = parts[1]; // 路径

            size_t query_pos = request.path.find('?');
            if (query_pos != std::string::npos) {
                // 我们这里先不解析查询参数，只保留路径部分
                // 实际上应该解析查询参数，但为了简化先这样
                request.path = request.path.substr(0, query_pos);
            }

            request.version = parts[2];
            return true;
        }


        /**
     * 解析头部，如： "Host: localhost:8080"
     * 头部行:
     * 存储对象:
     * 
     */
        static bool parse_header_line(const std::string& line, HttpRequest& request)
        {
            size_t colon_pos = line.find(":");
            if (colon_pos == std::string::npos) {
                SYLAR_LOG_ERROR(g_logger) << "解析头部错误";
                return false;
            }

            std::string key = trim(line.substr(0, colon_pos));
            std::string value = trim(line.substr(colon_pos + 1));

            key = to_lower(key);

            request.headers[key] = value;
            return true;
        }

    private:
        /**
     * 去除字符串两边的空白部分
     * 需要操作的字符串:
     * 
     */
        static std::string trim(const std::string& str) {
            auto start = str.begin();
            auto end = str.end();

            // 找到第一个非空白字符，isspace用于检查是否有空白
            while (start != end && std::isspace(*start)) {
                ++start;
            }

            // 找到最后一个非空白字符
            do {
                --end;
            } while (std::distance(start, end) > 0 && std::isspace(*end));

            return std::string(start, end + 1);
        }

        /**
     * 将大写字符串转换成小写
     * 需要转换的字符串:
     * 返回: 全部为小写的字符串
     */
        static std::string to_lower(const std::string& str) {
            std::string result = str;
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return result;
        }

        /**
     * 工具函数，用来分割字符串
     * 需要分割的字符串string:
     * 分割的标准，比如"\r\n"表示以这个为标准进行分割:
     * 返回: 返回分割后的数组
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
