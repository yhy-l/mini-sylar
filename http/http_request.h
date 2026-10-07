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
     * @brief HTTP 头部容器
     *
     * HTTP 报文头部通常只有几条到十几条。用 unordered_map 的代价是：
     * 每插入一条都要算一次哈希、单独申请一个节点，遍历时缓存也不连续。
     * 这里用一个小 vector 做线性查找：一次连续分配、遍历友好，
     * 头部条数少时比哈希表更快，并且省掉每请求若干次节点分配。
     */
    class HeaderMap
    {
    public:
        using Item = std::pair<std::string, std::string>;
        using iterator = std::vector<Item>::iterator;
        using const_iterator = std::vector<Item>::const_iterator;

        HeaderMap() { m_items.reserve(4); }

        /// 取某个头部的值；不存在则插入一个空值（和 map 的 operator[] 语义一致）
        std::string& operator[](std::string key)
        {
            auto it = find(key);
            if (it != m_items.end()) { return it->second; }
            m_items.emplace_back(std::move(key), std::string());
            return m_items.back().second;
        }

        iterator find(std::string_view key)
        {
            for (auto it = m_items.begin(); it != m_items.end(); ++it) {
                if (it->first == key) { return it; }
            }
            return m_items.end();
        }

        const_iterator find(std::string_view key) const
        {
            for (auto it = m_items.begin(); it != m_items.end(); ++it) {
                if (it->first == key) { return it; }
            }
            return m_items.end();
        }

        bool contains(std::string_view key) const { return find(key) != m_items.end(); }

        iterator begin() { return m_items.begin(); }
        iterator end() { return m_items.end(); }
        const_iterator begin() const { return m_items.begin(); }
        const_iterator end() const { return m_items.end(); }

        size_t size() const { return m_items.size(); }
        bool empty() const { return m_items.empty(); }
        void clear() { m_items.clear(); }

    private:
        std::vector<Item> m_items;
    };

    /**
 * http请求结构体，用于接受客户端的请求
 */
    struct HttpRequest
    {
        std::string method; //GET POST等方法
        std::string path; //请求路径 ，如:/index.html等
        std::string version; //版本

        //请求头部  key-request格式
        HeaderMap headers;

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
            return headers.contains(lower_key);
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
        HeaderMap headers;

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
            // 预估总长度，一次 reserve，避免多次扩容与临时 string 构造
            size_t estimate = version.size() + status_text.size() + 16 + body.size() + 2;
            for (const auto& kv : headers) {
                estimate += kv.first.size() + kv.second.size() + 4;
            }

            std::string respone;
            respone.reserve(estimate);

            // 状态行：HTTP/1.1 200 OK\r\n
            respone.append(version);
            respone.push_back(' ');
            respone.append(std::to_string(status_code));
            respone.push_back(' ');
            respone.append(status_text);
            respone.append("\r\n");

            // 头部
            for (const auto& kv : headers) {
                respone.append(kv.first);
                respone.append(": ");
                respone.append(kv.second);
                respone.append("\r\n");
            }

            respone.append("\r\n");
            respone.append(body);
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
