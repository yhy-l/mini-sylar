#include "mq_log_sender.h"
#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <amqp_framing.h>
#include <cstdio>
#include <cstring>

namespace sylar {

MqLogSender::MqLogSender()
    : m_conn(nullptr)
    , m_channel(1)
    , m_port(5672)
    , m_ready(false)
{}

MqLogSender::~MqLogSender()
{
    close();
}

static bool check_reply(amqp_connection_state_t conn, const char* what)
{
    amqp_rpc_reply_t reply = amqp_get_rpc_reply(conn);
    if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
        if (reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION) {
            fprintf(stderr, "[MqLogSender] %s: %s\n", what, amqp_error_string2(reply.library_error));
        } else if (reply.reply_type == AMQP_RESPONSE_SERVER_EXCEPTION
                   && reply.reply.id == AMQP_CHANNEL_CLOSE_METHOD) {
            amqp_channel_close_t* m = static_cast<amqp_channel_close_t*>(reply.reply.decoded);
            fprintf(stderr, "[MqLogSender] %s: channel_close code=%u text=%.*s\n",
                    what, m->reply_code, (int)m->reply_text.len, (char*)m->reply_text.bytes);
        } else {
            fprintf(stderr, "[MqLogSender] %s failed (reply_type=%d id=%d)\n", what, reply.reply_type, reply.reply.id);
        }
        return false;
    }
    return true;
}

bool MqLogSender::init(const std::string& host, int port, const std::string& queue)
{
    m_host = host;
    m_port = port;
    m_queue = queue;
    m_ready = ensureConnection();
    return m_ready;
}

bool MqLogSender::ensureConnection()
{
    if (m_conn != nullptr) {
        amqp_connection_close(static_cast<amqp_connection_state_t>(m_conn), AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(static_cast<amqp_connection_state_t>(m_conn));
        m_conn = nullptr;
    }

    amqp_connection_state_t conn = amqp_new_connection();
    amqp_socket_t* socket = amqp_tcp_socket_new(conn);
    if (!socket) {
        fprintf(stderr, "[MqLogSender] amqp_tcp_socket_new failed\n");
        return false;
    }
    if (amqp_socket_open(socket, m_host.c_str(), m_port) != AMQP_STATUS_OK) {
        fprintf(stderr, "[MqLogSender] connect %s:%d failed\n", m_host.c_str(), m_port);
        amqp_destroy_connection(conn);
        return false;
    }
    if (amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
                   "guest", "guest").reply_type != AMQP_RESPONSE_NORMAL) {
        fprintf(stderr, "[MqLogSender] login failed\n");
        amqp_destroy_connection(conn);
        return false;
    }
    amqp_channel_open(conn, m_channel);
    if (!check_reply(conn, "channel_open")) {
        amqp_destroy_connection(conn);
        return false;
    }

    // 声明持久队列（幂等）
    amqp_queue_declare(conn, m_channel,
                       amqp_cstring_bytes(m_queue.c_str()),
                       0 /*passive*/, 1 /*durable*/, 0, 0, amqp_empty_table);
    if (!check_reply(conn, "queue_declare")) {
        amqp_destroy_connection(conn);
        return false;
    }

    m_conn = conn;
    return true;
}

bool MqLogSender::publish(const std::string& body)
{
    if (body.empty()) {
        return true;
    }
    if (m_conn == nullptr) {
        m_ready = ensureConnection();
        if (!m_ready) {
            return false;
        }
    }

    amqp_bytes_t payload;
    payload.len = body.size();
    payload.bytes = const_cast<char*>(body.data());

    if (amqp_basic_publish(static_cast<amqp_connection_state_t>(m_conn), m_channel,
                           amqp_cstring_bytes(""), amqp_cstring_bytes(m_queue.c_str()),
                           0, 0, nullptr, payload) != AMQP_STATUS_OK) {
        // 可能连接已断，重连一次再试
        fprintf(stderr, "[MqLogSender] publish failed, reconnect...\n");
        m_ready = ensureConnection();
        if (!m_ready) {
            return false;
        }
        if (amqp_basic_publish(static_cast<amqp_connection_state_t>(m_conn), m_channel,
                               amqp_cstring_bytes(""), amqp_cstring_bytes(m_queue.c_str()),
                               0, 0, nullptr, payload) != AMQP_STATUS_OK) {
            fprintf(stderr, "[MqLogSender] publish retry failed\n");
            return false;
        }
    }
    amqp_maybe_release_buffers(static_cast<amqp_connection_state_t>(m_conn));
    return true;
}

void MqLogSender::close()
{
    if (m_conn != nullptr) {
        amqp_connection_close(static_cast<amqp_connection_state_t>(m_conn), AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(static_cast<amqp_connection_state_t>(m_conn));
        m_conn = nullptr;
    }
    m_ready = false;
}

} // namespace sylar
