#pragma once
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include "log.h"

namespace sylar {

    extern sylar::Logger::ptr g_logger;

    /**
     * 读取本地图片文件
     * filepath文件地址:
     * 
     * std::vector<unsigned char> image_data = sylar::read_image_file("photo.jpg");
     */
    std::vector<unsigned char> read_image_file(const std::string& filepath)
    {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);

        if (!file.is_open()) {
            SYLAR_LOG_ERROR(g_logger) << "错误：无法打开图片文件" << filepath;
            return {};
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (size <= 0) {
            SYLAR_LOG_ERROR(g_logger) << "错误：图片文件大小为0或无效";
            return {};
        }

        std::vector<unsigned char> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            SYLAR_LOG_ERROR(g_logger) << "错误：读取图片文件失败";
            return {};
        }

        return buffer;
    }

    /**
     * 读取本地视频文件（处理大文件）
     * filepath文件地址:
     * chunk_size: 每次读取的块大小，默认1MB
     * 
     */
    std::vector<unsigned char> read_video_file(const std::string& filepath, size_t chunk_size = 1024 * 1024)
    {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);

        if (!file.is_open()) {
            std::cerr << "错误：无法打开视频文件 " << filepath << std::endl;
            return {};
        }

        // 获取文件大小
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (size <= 0) {
            std::cerr << "警告：视频文件大小为0" << std::endl;
            return {};
        }

        SYLAR_LOG_INFO(g_logger) << "开始读取视频文件：" << filepath << " ,大小：" << size << "字节";


        // 分配内存
        std::vector<unsigned char> buffer;
        buffer.resize(size);

        std::vector<char> chunk(chunk_size);
        size_t total_read = 0;
        size_t chunk_number = 0;

        while (file) {
            file.read(chunk.data(), chunk.size());
            size_t bytes_read = file.gcount();

            if (bytes_read > 0) {
                buffer.insert(buffer.end(),
                              reinterpret_cast<char*>(chunk.data()),
                              reinterpret_cast<char*>(chunk.data()) + bytes_read);

                total_read += bytes_read;
                chunk_number++;

                if(chunk_number % 10 == 0)
                {
                    SYLAR_LOG_DEBUG(g_logger) << "读取进度: " << total_read << "/"
                                              << size << " 字节 ("
                                              << (total_read * 100 / size) << "%)";

                }

            }

            if (bytes_read < chunk.size()) {
                break;  // 已读取完文件
            }

        }

        SYLAR_LOG_INFO(g_logger) << "视频文件读取完成: " << total_read << " 字节";

        if (total_read != size) {
            SYLAR_LOG_WARN(g_logger) << "视频文件可能读取不完整: 期望 "
                                     << size << " 字节, 实际 " << total_read << " 字节";
        }

        return buffer;
    }

    /**
     * 读取本地文件（通用）
     * filepath文件地址:
     * 
     */
    std::vector<unsigned char> read_binary_file(const std::string& filepath) {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);

        if (!file.is_open()) {
            int error_code = errno;
            SYLAR_LOG_ERROR(g_logger) << "错误：无法打开文件：" << filepath
                                      << "。系统错误: " << strerror(error_code)
                                      << " (错误码: " << error_code << ")";
            return {};
        }

        // 获取文件大小
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (size <= 0) {
            std::cerr << "警告：文件大小为0" << std::endl;
            return {};
        }

        // 读取到vector
        std::vector<unsigned char> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            std::cerr << "错误：读取文件失败 " << filepath << std::endl;
            return {};
        }

        return buffer;
    }


    /**
     * 使用流式传输传递超大文件
     */
    class VideoStreamReader
    {
    private:
        std::ifstream file_;    //文件流
        size_t file_size_;      //文件大小
        size_t current_pos_;    //当前文件位置
        bool is_open_;

    public:
        VideoStreamReader()
            : file_size_(0)
            , current_pos_(0)
            , is_open_(false)
        {}

        /**
         * 打开大文件
         * filepath:
         * 
         */
        bool open(const std::string& filepath)
        {
            if (is_open_) { close(); }

            file_.open(filepath, std::ios::binary | std::ios::ate);
            if (!file_.is_open()) {
                SYLAR_LOG_ERROR(g_logger) << "错误：无法打开文件：" << filepath;
                is_open_ = false;
                return false;
            }

            file_size_ = file_.tellg();
            file_.seekg(0, std::ios::beg);
            current_pos_ = 0;
            is_open_ = true;

            SYLAR_LOG_INFO(g_logger) << "打开视频文件：" << filepath << "，大小：" << file_size_ << "字节";

            return true;
        }

        /**
         * 读取下一个数据块
         * chunk_size块大小:
         * 返回: 数据块，如果读取失败或者结束返回空
         */
        std::vector<unsigned char> read_chunk(size_t chunk_size = 1024 * 1024)
        {
            if (!is_open_) {
                SYLAR_LOG_ERROR(g_logger) << "视频文件未打开";
                return {};
            }

            if (eof()) {
                SYLAR_LOG_DEBUG(g_logger) << "已经到达末尾";
                return {};
            }

            size_t remaining = file_size_ - current_pos_;  //剩余待读取
            size_t to_read = (chunk_size < remaining) ? chunk_size : remaining;

            std::vector<unsigned char> chunk(to_read);
            file_.read(reinterpret_cast<char*>(chunk.data()), to_read);
            size_t bytes_read = file_.gcount();

            if (bytes_read != to_read) {
                SYLAR_LOG_WARN(g_logger) << "读取不完整: 期望 " << to_read << " 字节, 实际 " << bytes_read << " 字节";
            }

            SYLAR_LOG_DEBUG(g_logger) << "读取块: " << bytes_read << " 字节, "
                                      << "总进度: " << current_pos_ << "/" << file_size_ << " ("
                                      << (current_pos_ * 100 / file_size_) << "%)";

            return chunk;
        }


        /**
         * 读取所有数据
         * 
         */
        std::vector<unsigned char> read_all()
        {
            if (!is_open_) { return {}; }

            size_t remaining = file_size_ - current_pos_;
            if (remaining == 0) { return {}; }

            return read_chunk(remaining);
        }


        /**
         * 是否已到达文件末尾
         */
        bool eof() const {
            return current_pos_ >= file_size_;
        }


        /**
         * 获取当前读取位置
         */
        size_t tell() const {
            return current_pos_;
        }

        /**
         * 跳转到指定位置
         * pos: 位置
         */
        bool seek(size_t pos) {
            if (!is_open_) {
                return false;
            }

            if (pos > file_size_) {
                SYLAR_LOG_ERROR(g_logger) << "跳转位置超过文件大小: " << pos << " > " << file_size_;
                return false;
            }

            file_.seekg(pos, std::ios::beg);
            if (!file_) {
                SYLAR_LOG_ERROR(g_logger) << "跳转失败: " << pos;
                return false;
            }

            current_pos_ = pos;
            SYLAR_LOG_DEBUG(g_logger) << "跳转到位置: " << pos;
            return true;
        }

        /**
         * 重新开始读取（回到文件开头）
         */
        bool rewind() {
            return seek(0);
        }

        /**
         * 获取文件大小
         */
        size_t size() const {
            return file_size_;
        }

        /**
         * 获取剩余字节数
         */
        size_t remaining() const {
            if (!is_open_) {
                return 0;
            }
            return file_size_ - current_pos_;
        }

        /**
         * 文件是否已打开
         */
        bool is_open() const {
            return is_open_;
        }

        /**
         * 关闭文件
         */
        void close() {
            if (file_.is_open()) {
                file_.close();
                SYLAR_LOG_DEBUG(g_logger) << "关闭视频文件";
            }
            is_open_ = false;
            file_size_ = 0;
            current_pos_ = 0;
        }

        ~VideoStreamReader() {
            close();
        }

    };

}

