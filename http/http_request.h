#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cctype>
#include "../log.h"

namespace sylar {

    extern sylar::Logger::ptr g_logger;

    /**
 * http请求结构体，用于接受客户端的请求
 */
    struct HttpRequest
    {
        std::string method; //GET POST等方法
        std::string path; //请求路径 ，如:/index.html等
        std::string version; //版本

        //请求头部  key-request格式
        std::unordered_map<std::string, std::string> headers;

        std::string body; //请求体

        enum ParseState {
            PARSE_START, // 开始解析
            PARSE_HEADERS, // 解析头部
            PARSE_BODY, // 解析主体
            PARSE_COMPLETE, // 解析完成
            PARSE_ERROR // 解析错误
        };

        ParseState state = PARSE_START;


        /**
     * 寻找一个头部
     * key:
     * 返回: value
     */
        std::string get_header(const std::string& key) const
        {
            std::string lower_key = key;
            std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            auto it = headers.find(lower_key);
            if (it != headers.end()) {
                return it->second;
            } else {
                return "";
            }

        }

        /**
     * 判断一个头部是否存在
     * key:
     * 
     */
        bool has_header(const std::string& key) const
        {
            std::string lower_key = key;
            std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return headers.find(lower_key) != headers.end();
        }

        /**
     * 获取头部中的content-length
     * 返回: 0表示没有或者失败
     */
        size_t get_content_length() const
        {
            auto it = headers.find("content-length");
            if (it != headers.end()) {
                try {
                    return std::stoul(it->second);
                } catch (...) {
                    SYLAR_LOG_ERROR(g_logger) << "获取头部content-length失败";
                    return 0;
                }
            }
            SYLAR_LOG_ERROR(g_logger) << "获取头部content-length失败";
            return 0;
        }

        /**
     * 判断是否为长久连接
     * 返回: 1表示是
     */
        bool is_keep_alive() const
        {
            auto it = headers.find("connection");
            if (it != headers.end()) {
                std::string value = it->second;
                std::transform(value.begin(), value.end(), value.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                return value == "keep-alive" || (version == "HTTP/1.1" && value != "close");
            }

            return version == "HTTP/1.1";
        }
    };


    struct HttpResponse
    {
        std::string version = "HTTP/1.1"; //版本
        int status_code = 200; //状态码
        std::string status_text = "OK"; //状态文本

        // 头部
        std::unordered_map<std::string, std::string> headers;

        // 响应体
        std::string body;

        // 默认头部配置
        HttpResponse()
        {
            headers["Server"] = "MyHttpServer/1.0";
            headers["Connection"] = "close";
        }

        /**
     * 返回响应的http字符串序列
     * 返回: string
     */
        std::string to_string() const
        {
            std::string respone;

            //  状态行：HTTP/1.1 200 OK\r\n
            respone = version + " " + std::to_string(status_code) + " " + status_text + "\r\n";

            //  头部
            for (const auto& [key, value] : headers) {
                respone += key + ": " + value + "\r\n";
            }

            // 空行
            respone += "\r\n";

            //  响应体
            respone += body;

            return respone;
        }

        /**
     * 设置内容类型
     * type:
     */
        void set_content_type(const std::string& type) { headers["Content-Type"] = type; }

        /**
     * 手动设置内容大小
     * length:
     */
        void set_content_length(size_t length) { headers["Content-Length"] = std::to_string(length); }

        /**
     * 自动设置内容大小
     */
        void auto_set_content_length() { headers["Content-Length"] = std::to_string(body.size()); }


        /**
     * 创建纯文本的响应
     * text:
     * 
     */
        static HttpResponse make_text_response(const std::string& text)
        {
            HttpResponse res;
            res.set_content_type("text/plain; charset = utf-8");
            res.body = text;
            res.auto_set_content_length();
            return res;
        }

        /**
     * 创建html的响应
     * html:
     * 
     */
        static HttpResponse make_html_response(const std::string& html)
        {
            HttpResponse res;
            res.set_content_type("text/html; charset=utf-8");
            res.body = html;
            res.auto_set_content_length();
            return res;
        }

        /**
     * 创建json的响应
     * json:
     * 
     */
        static HttpResponse make_json_response(const std::string& json)
        {
            HttpResponse res;
            res.set_content_type("application/json; charset=utf-8");
            res.body = json;
            res.auto_set_content_length();
            return res;
        }

        /**
     * 创建错误的响应
     * code:
     * messgae:
     * 
     */
        static HttpResponse make_error_response(int code, const std::string& message)
        {
            HttpResponse res;
            res.status_code = code;
            res.status_text = get_status_text(code);

            std::string html = "<html><body><h1>" + std::to_string(code) + " " + res.status_text + "</h1><p>" + message
                               + "</p></body></html>";

            res.set_content_type("text/html; charset = utf-8");
            res.body = html;
            res.auto_set_content_length();
            return res;
        }

    private:
        // 获取状态码对应的文本
        static std::string get_status_text(int code) {
            switch (code) {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 500: return "Internal Server Error";
            case 503: return "Service Unavailable";
            default: return "Unknown";
            }
        }

    };


}
