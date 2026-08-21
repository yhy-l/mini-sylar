#pragma once
#include <string>

namespace sylar {

/**
 * RabbitMQ 日志发送器（生产者）
 * 封装 rabbitmq-c，向指定队列发布消息。
 * 连接失败/发布失败都不会抛异常，由调用方决定如何处理。
 */
class MqLogSender
{
public:
    MqLogSender();
    ~MqLogSender();

    /**
     * 连接RabbitMQ并声明队列
     * host: 服务地址
     * port: AMQP端口（默认5672）
     * queue: 队列名（持久队列）
     * 返回: 是否成功
     */
    bool init(const std::string& host, int port, const std::string& queue);

    /**
     * 发布一条消息
     * body: 消息体（如多行JSON）
     * 返回: 是否成功（失败时内部尝试重连）
     */
    bool publish(const std::string& body);

    void close();

private:
    bool ensureConnection();

    void* m_conn;       // amqp_connection_state_t（C指针，避免头文件暴露C API）
    int   m_channel;
    std::string m_host;
    int   m_port;
    std::string m_queue;
    bool  m_ready;
};

} // namespace sylar
