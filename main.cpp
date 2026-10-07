// // test/test_socket.cpp
// #include "socket.h"
// #include "iomanager.h"
// #include "log.h"
// #include <iostream>
// #include <thread>
// #include "task.h"
// #include "tcpserver.h"

// sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

// // void test_basic_socket() {
// //     SYLAR_LOG_INFO(g_logger) << "=== 测试基本Socket功能 ===";

// //     {
// //         // 创建TCP socket
// //         auto socket = sylar::Socket::CreateTCPSocket();
// //         SYLAR_LOG_INFO(g_logger) << "创建TCP socket: " << socket->toString();

// //         // 绑定本地地址
// //         sylar::IPv4Address::ptr addr = sylar::IPv4Address::Create("127.0.0.1", 8080);
// //         if (socket->bind(addr)) {
// //             SYLAR_LOG_INFO(g_logger) << "绑定地址成功: " << addr->toString();
// //         } else {
// //             SYLAR_LOG_ERROR(g_logger) << "绑定地址失败";
// //         }

// //         // 监听
// //         if (socket->listen(10)) {
// //             SYLAR_LOG_INFO(g_logger) << "监听成功";
// //         } else {
// //             SYLAR_LOG_ERROR(g_logger) << "监听失败";
// //         }

// //         // 获取本地地址
// //         auto local = socket->getLocalAddress();
// //         if (local) {
// //             SYLAR_LOG_INFO(g_logger) << "本地地址: " << local->toString();
// //         }

// //         socket->close();
// //     }

// //     SYLAR_LOG_INFO(g_logger) << "基本Socket测试完成";
// // }


// // 客户端任务函数
// sylar::Task client_task(sylar::Socket::ptr client, sylar::IPv4Address::ptr client_addr) {
//     SYLAR_LOG_INFO(g_logger) << "客户端开始连接...";

//     if (client->connect(client_addr)) {
//         SYLAR_LOG_INFO(g_logger) << "客户端连接成功";

//         // 发送数据
//         std::string msg = "Hello Server!";
//         ssize_t n = client->send(msg.c_str(), msg.size());
//         SYLAR_LOG_INFO(g_logger) << "客户端发送 " << n << " 字节: " << msg;

//         // 接收响应
//         char buffer[256];
//         n = client->recv(buffer, sizeof(buffer)-1);
//         if (n > 0) {
//             buffer[n] = '\0';
//             SYLAR_LOG_INFO(g_logger) << "客户端收到响应: " << buffer;
//         }
//     } else {
//         SYLAR_LOG_ERROR(g_logger) << "客户端连接失败";
//     }

//     client->close();
//     co_return;
// }

// // 服务器任务函数
// sylar::Task server_task(sylar::IOManager* iom,sylar::Socket::ptr server) {
//     SYLAR_LOG_INFO(g_logger) << "服务器等待连接...";

//     bool ready = co_await iom->waitReadAsync(server->getSocket(), 5000);

//     if(ready)
//     {
//         auto client_sock = server->accept();
//         if (client_sock) {
//             SYLAR_LOG_INFO(g_logger) << "服务器接受连接: " << client_sock->toString();

//             // 接收数据
//             char buffer[256];
//             ssize_t n = client_sock->recv(buffer, sizeof(buffer)-1);
//             if (n > 0) {
//                 buffer[n] = '\0';
//                 SYLAR_LOG_INFO(g_logger) << "服务器收到: " << buffer;

//                 // 发送响应
//                 std::string response = "Hello Client!";
//                 n = client_sock->send(response.c_str(), response.size());
//                 SYLAR_LOG_INFO(g_logger) << "服务器发送响应: " << response;
//             }

//             client_sock->close();
//         } else {
//             SYLAR_LOG_ERROR(g_logger) << "服务器接受连接失败";
//         }
//     }

//     server->close();
//     co_return;
// }

// void test_socket_connect() {
//     SYLAR_LOG_INFO(g_logger)<< "=== 测试Socket连接 ===";

//         sylar::IOManager iom;

//         // 创建服务器socket
//         auto server = sylar::Socket::CreateTCPSocket();
//         sylar::IPv4Address::ptr server_addr = sylar::IPv4Address::Create("127.0.0.1", 8888);

//         if (!server->bind(server_addr)) {
//             SYLAR_LOG_ERROR(g_logger) << "服务器绑定失败";
//             return;
//         }

//         if (!server->listen()) {
//             SYLAR_LOG_ERROR(g_logger) << "服务器监听失败";
//             return;
//         }

//         SYLAR_LOG_INFO(g_logger) << "服务器启动: " << server->getLocalAddress()->toString();

//         iom.schedule(server_task(&iom,server));

//         // // 创建客户端socket
//         // auto client = sylar::Socket::CreateTCPSocket();
//         // sylar::IPv4Address::ptr client_addr = sylar::IPv4Address::Create("127.0.0.1", 8888);

//         // // 提交任务
//         // iom.schedule(client_task(client, client_addr));


//         // 等待处理
//         std::this_thread::sleep_for(std::chrono::milliseconds(100000));

//         SYLAR_LOG_INFO(g_logger) << "Socket连接测试完成";
// }

// void printServerConnectInfo(int port) {
//     SYLAR_LOG_INFO(g_logger) << "========================================";
//     SYLAR_LOG_INFO(g_logger) << "服务器启动成功！";
//     SYLAR_LOG_INFO(g_logger) << "========================================";

//     // 使用你的函数获取网卡地址
//     std::multimap<std::string, std::pair<sylar::Address::ptr, uint32_t>> result;

//     if (sylar::Address::GetInterfaceAddresses(result, AF_INET)) {  // 只获取IPv4地址
//         SYLAR_LOG_INFO(g_logger) << "客户端可以使用以下地址连接：";

//         // 本地回环地址（固定）
//         SYLAR_LOG_INFO(g_logger) << "1. 本地访问：";
//         SYLAR_LOG_INFO(g_logger) << "   - 127.0.0.1:" << port << " (本机回环)";
//         SYLAR_LOG_INFO(g_logger) << "   - localhost:" << port;

//         SYLAR_LOG_INFO(g_logger) << "2. 局域网访问：";

//         // 遍历所有网卡地址
//         std::string last_ifname = "";
//         for (auto& it : result) {
//             const std::string& ifname = it.first;  // 网卡名称
//             sylar::Address::ptr addr = it.second.first;  // 地址对象
//             uint32_t prefix_len = it.second.second;  // 子网掩码长度

//             // 获取IP字符串
//             std::string ip_str = addr->toString();

//             // 从字符串中提取IP（去掉端口部分）
//             std::string ip = ip_str;
//             size_t colon_pos = ip_str.find_last_of(':');
//             if (colon_pos != std::string::npos) {
//                 ip = ip_str.substr(0, colon_pos);
//             }

//             // 排除回环地址
//             if (ip == "127.0.0.1") {
//                 continue;
//             }

//             // 排除docker、虚拟网卡等（可选）
//             if (ifname.find("docker") != std::string::npos ||
//                 ifname.find("virbr") != std::string::npos ||
//                 ifname.find("veth") != std::string::npos) {
//                 continue;
//             }

//             // 按网卡分组显示
//             if (ifname != last_ifname) {
//                 SYLAR_LOG_INFO(g_logger) << "   [" << ifname << "] 网卡:";
//                 last_ifname = ifname;
//             }

//             SYLAR_LOG_INFO(g_logger) << "     - " << ip << ":" << port;

//             // 显示子网信息（可选）
//             if (prefix_len != ~0u) {
//                 SYLAR_LOG_INFO(g_logger) << "       子网掩码: /" << prefix_len;
//             }
//         }
//     } else {
//         SYLAR_LOG_WARN(g_logger) << "无法获取网卡地址信息";
//     }

//     SYLAR_LOG_INFO(g_logger) << "========================================";
// }


// sylar::Task run(sylar::IOManager *iomptr)
// {
//     sylar::TcpServer::ptr tcpserver(new sylar::TcpServer(iomptr, iomptr)); //传递iom
//     sylar::IPv4Address::ptr server_addr = sylar::IPv4Address::Create("0.0.0.0", 8080);


//     std::vector<sylar::Address::ptr> addrs;
//     addrs.push_back(server_addr);
//     std::vector<sylar::Address::ptr> fails;
//     while (!tcpserver->bind(addrs, fails)) {
//         SYLAR_LOG_INFO(g_logger) << "绑定失败";
//         co_return;
//     }

//     SYLAR_LOG_INFO(g_logger) << "绑定成功, " << tcpserver->toString();

//     // printServerConnectInfo(8080);

//     tcpserver->start();


//     while (!tcpserver->isStop()) {
//         co_await iomptr->waitReadAsync(0, 1000);  // 确保tcpserver不被销毁，不能结束
//     }

//     co_return;
// }


// void tcp_serverTest()
// {
//     sylar::IOManager iom;
//     iom.schedule(run(&iom));

//     std::this_thread::sleep_for(std::chrono::milliseconds(10000000));
// }


// int main() {
//     sylar::Logger::ptr logger = sylar::LoggerManager::getInstance().getRoot();
//     // logger->setLevel(sylar::LogLevel::ERROR);

//     tcp_serverTest();
//     // test_socket_connect();

//     return 0;
// }



// main.cpp
#include "http/http_server.h"
#include "log.h"
#include "iomanager.h"
#include "address.h"
#include <iostream>
#include <memory>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cstdlib>

namespace sylar {
    Logger::ptr g_logger = SYLAR_LOG_ROOT();
}

// 信号处理器只负责设置这个标志（async-signal-safe），
// 真正的关停（stop、join线程）放到主线程做，避免在信号上下文里调用非安全操作
static std::atomic<bool> g_stop_requested{false};

// 端口支持命令行覆盖，便于多实例部署（nginx 负载均衡到多个后端）
static int g_http_port = 8080;
static int g_tcp_port = 8081;

// 打印服务器连接信息
void printServerConnectInfo(int port) {
    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";
    SYLAR_LOG_INFO(sylar::g_logger) << "HTTP服务器启动成功！";
    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";

    // 使用你的函数获取网卡地址
    std::multimap<std::string, std::pair<sylar::Address::ptr, uint32_t>> result;

    if (sylar::Address::GetInterfaceAddresses(result, AF_INET)) {  // 只获取IPv4地址
        SYLAR_LOG_INFO(sylar::g_logger) << "客户端可以使用以下地址连接：";

        // 本地回环地址（固定）
        SYLAR_LOG_INFO(sylar::g_logger) << "1. 本地访问：";
        SYLAR_LOG_INFO(sylar::g_logger) << "   - 127.0.0.1:" << port << " (本机回环)";
        SYLAR_LOG_INFO(sylar::g_logger) << "   - localhost:" << port;

        SYLAR_LOG_INFO(sylar::g_logger) << "2. 局域网访问：";

        // 遍历所有网卡地址
        std::string last_ifname = "";
        for (auto& it : result) {
            const std::string& ifname = it.first;  // 网卡名称
            sylar::Address::ptr addr = it.second.first;  // 地址对象
            uint32_t prefix_len = it.second.second;  // 子网掩码长度

            // 获取IP字符串
            std::string ip_str = addr->toString();

            // 从字符串中提取IP（去掉端口部分）
            std::string ip = ip_str;
            size_t colon_pos = ip_str.find_last_of(':');
            if (colon_pos != std::string::npos) {
                ip = ip_str.substr(0, colon_pos);
            }

            // 排除回环地址
            if (ip == "127.0.0.1") {
                continue;
            }

            // 排除docker、虚拟网卡等（可选）
            if (ifname.find("docker") != std::string::npos ||
                ifname.find("virbr") != std::string::npos ||
                ifname.find("veth") != std::string::npos) {
                continue;
            }

            // 按网卡分组显示
            if (ifname != last_ifname) {
                SYLAR_LOG_INFO(sylar::g_logger) << "   [" << ifname << "] 网卡:";
                last_ifname = ifname;
            }

            SYLAR_LOG_INFO(sylar::g_logger) << "     - " << ip << ":" << port;

            // 显示子网信息（可选）
            if (prefix_len != ~0u) {
                SYLAR_LOG_INFO(sylar::g_logger) << "       子网掩码: /" << prefix_len;
            }
        }
    } else {
        SYLAR_LOG_WARN(sylar::g_logger) << "无法获取网卡地址信息";
    }

    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";
    SYLAR_LOG_INFO(sylar::g_logger) << "测试接口:";
    SYLAR_LOG_INFO(sylar::g_logger) << "  - GET  /             首页";
    SYLAR_LOG_INFO(sylar::g_logger) << "  - GET  /about.html   关于页面（静态文件）";
    SYLAR_LOG_INFO(sylar::g_logger) << "  - GET  /css/style.css 样式（静态文件）";
    SYLAR_LOG_INFO(sylar::g_logger) << "  - GET  /js/app.js    脚本（静态文件）";
    SYLAR_LOG_INFO(sylar::g_logger) << "  - GET  /api/hello    JSON接口";
    SYLAR_LOG_INFO(sylar::g_logger) << "  - GET  /api/time     时间接口";
    SYLAR_LOG_INFO(sylar::g_logger) << "  - POST /api/echo     回显接口";
    SYLAR_LOG_INFO(sylar::g_logger) << "  - GET  /text         纯文本接口";
    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";
}

void printServerConnectInfoTCP(int port) {
    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";
    SYLAR_LOG_INFO(sylar::g_logger) << "服务器启动成功！";
    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";

    // 使用你的函数获取网卡地址
    std::multimap<std::string, std::pair<sylar::Address::ptr, uint32_t>> result;

    if (sylar::Address::GetInterfaceAddresses(result, AF_INET)) {  // 只获取IPv4地址
        SYLAR_LOG_INFO(sylar::g_logger) << "客户端可以使用以下地址连接：";

        // 本地回环地址（固定）
        SYLAR_LOG_INFO(sylar::g_logger) << "1. 本地访问：";
        SYLAR_LOG_INFO(sylar::g_logger) << "   - 127.0.0.1:" << port << " (本机回环)";
        SYLAR_LOG_INFO(sylar::g_logger) << "   - localhost:" << port;

        SYLAR_LOG_INFO(sylar::g_logger) << "2. 局域网访问：";

        // 遍历所有网卡地址
        std::string last_ifname = "";
        for (auto& it : result) {
            const std::string& ifname = it.first;  // 网卡名称
            sylar::Address::ptr addr = it.second.first;  // 地址对象
            uint32_t prefix_len = it.second.second;  // 子网掩码长度

            // 获取IP字符串
            std::string ip_str = addr->toString();

            // 从字符串中提取IP（去掉端口部分）
            std::string ip = ip_str;
            size_t colon_pos = ip_str.find_last_of(':');
            if (colon_pos != std::string::npos) {
                ip = ip_str.substr(0, colon_pos);
            }

            // 排除回环地址
            if (ip == "127.0.0.1") {
                continue;
            }

            // 排除docker、虚拟网卡等（可选）
            if (ifname.find("docker") != std::string::npos ||
                ifname.find("virbr") != std::string::npos ||
                ifname.find("veth") != std::string::npos) {
                continue;
            }

            // 按网卡分组显示
            if (ifname != last_ifname) {
                SYLAR_LOG_INFO(sylar::g_logger) << "   [" << ifname << "] 网卡:";
                last_ifname = ifname;
            }

            SYLAR_LOG_INFO(sylar::g_logger) << "     - " << ip << ":" << port;

            // 显示子网信息（可选）
            if (prefix_len != ~0u) {
                SYLAR_LOG_INFO(sylar::g_logger) << "       子网掩码: /" << prefix_len;
            }
        }
    } else {
        SYLAR_LOG_WARN(sylar::g_logger) << "无法获取网卡地址信息";
    }

    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";
}

// 配置一个HTTP服务器实例（路由 / 静态目录 / Redis限流统计）
void setup_http_server(sylar::HttpServer::ptr httpserver, sylar::RedisClient::ptr redis) {
    // 静态文件目录：兼容从项目根目录或build/目录启动
    httpserver->setStaticDir(std::filesystem::exists("web") ? "web" : "../web");

    // 注册示例路由
    httpserver->register_example_routes();

    // 限流演示接口：每IP每60秒最多10次，超限返回429
    httpserver->get("/api/limited", [](const sylar::HttpRequest&) {
        std::string json = R"({"status":"ok","msg":"限流演示接口：每IP每60秒最多10次"})";
        return sylar::HttpResponse::make_json_response(json);
    });

    // Redis：接口限流 + 实时统计
    // 所有实例共享一个RedisClient（独立线程执行）；任务携带ownerIom，唤醒时回到各自实例
    httpserver->setRedisClient(redis);
    httpserver->setRateLimit("/api/limited", 10, 60);
    httpserver->setRateLimit("/api/echo", 100, 60);

    // HTTP_STATS=0 关闭Redis实时统计上报（压测/降级场景）
    if (const char* stats = std::getenv("HTTP_STATS")) {
        if (std::string(stats) == "0") { httpserver->setStatsEnabled(false); }
    }
}

// 每个HTTP实例对应一个协程：绑定地址 → 启动 → 等待停止 → 优雅关停
sylar::Task run_http_instance(sylar::IOManager* iom, sylar::HttpServer::ptr httpserver) {
    sylar::IPv4Address::ptr server_addr = sylar::IPv4Address::Create("0.0.0.0", g_http_port);

    std::vector<sylar::Address::ptr> addrs;
    addrs.push_back(server_addr);
    std::vector<sylar::Address::ptr> fails;

    if (!httpserver->bind(addrs, fails)) {
        SYLAR_LOG_ERROR(sylar::g_logger) << "HTTP实例绑定地址失败";
        co_return;
    }
    httpserver->start();

    // 挂起协程，保持server对象存活，直到本实例的IOManager停止
    co_await iom->waitStopAsync();
    httpserver->stop();
    co_return;
}

// 占位协程：让IO实例的线程启动并保持运行（该实例只处理被分发过来的连接）
sylar::Task hold_instance(sylar::IOManager* iom) {
    co_await iom->waitStopAsync();
    co_return;
}

//运行tcp服务器的协程任务
sylar::Task run(sylar::IOManager *iomptr)
{
    //配置tcp服务，传递iomptr作为调度器
    sylar::TcpServer::ptr tcpserver(new sylar::TcpServer(iomptr, iomptr)); //传递iom
    
    //配置地址address
    sylar::IPv4Address::ptr server_addr = sylar::IPv4Address::Create("0.0.0.0", g_tcp_port);

    std::vector<sylar::Address::ptr> addrs;
    addrs.push_back(server_addr);
    std::vector<sylar::Address::ptr> fails;
    while (!tcpserver->bind(addrs, fails)) {
        SYLAR_LOG_INFO(sylar::g_logger) << "绑定失败";
        co_return;
    }

    SYLAR_LOG_INFO(sylar::g_logger) << "绑定成功, " << tcpserver->toString();

    printServerConnectInfoTCP(g_tcp_port);

    tcpserver->start();


    // 挂起协程，保持tcpserver对象存活，直到IOManager停止
    co_await iomptr->waitStopAsync();

    // 优雅关停：关闭监听socket和所有活动客户端连接
    tcpserver->stop();

    co_return;
}

int main(int argc, char** argv)
{
    // 支持多实例部署：./mini-sylar [http端口] [tcp端口]
    if (argc > 1) g_http_port = std::atoi(argv[1]);
    if (argc > 2) g_tcp_port = std::atoi(argv[2]);

    sylar::g_logger = SYLAR_LOG_ROOT();
    // 日志级别：INFO（DEBUG太吵、WARN会滤掉MQ演示用的INFO日志）
    sylar::g_logger->setLevel(sylar::LogLevel::WARN);
    sylar::LoggerManager::getInstance().getLogger("system")->setLevel(sylar::LogLevel::WARN);

    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";
    SYLAR_LOG_INFO(sylar::g_logger) << "HTTP服务器启动";
    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";

    // 设置信号处理：只标记退出请求，不再在信号处理器里做任何非安全操作
    signal(SIGINT, [](int) { g_stop_requested = true; });
    signal(SIGTERM, [](int) { g_stop_requested = true; });
    // 网络写失败必须由 send 返回错误来处理，不能让 SIGPIPE 直接杀进程
    signal(SIGPIPE, SIG_IGN);

    // 多实例架构：N 个实例，每个 = 1个线程 + 1个独立epoll（多事件循环并行）
    // 连接分发采用"单监听 + 轮转分发"：只由 iom[0] 接受连接，
    // 再按轮转把连接交给各实例的 epoll 处理，避免 SO_REUSEPORT 哈希分发不均
    // （哈希分发会把多数连接集中到少数实例，其他实例空转）
    size_t kHttpInstances = 12;
    if (const char* env = std::getenv("HTTP_INSTANCES")) {
        size_t v = (size_t)std::atoi(env);
        if (v > 0) kHttpInstances = v;
    }
    std::vector<sylar::IOManager::ptr> httpIoms;
    sylar::RedisClient::ptr redis;

    for (size_t i = 0; i < kHttpInstances; ++i) {
        auto iom = std::make_shared<sylar::IOManager>(1, false);
        if (i == 0) {
            // Redis客户端被所有实例共享；任务携带ownerIom，唤醒时回到原实例
            const char* redis_host = std::getenv("REDIS_HOST");
            redis = std::make_shared<sylar::RedisClient>(iom.get());
            redis->init(redis_host ? redis_host : "127.0.0.1", 6379);
        }
        httpIoms.push_back(iom);

        if (i > 0) {
            // 其余实例：调度一个占位协程让线程启动并保持运行（只处理被分发的连接）
            iom->schedule(hold_instance(iom.get()));
        }
    }

    // 单一监听 + 轮转分发：accept 线程收到连接后按顺序分给各实例
    std::vector<sylar::IOManager*> ioTargets;
    for (auto& iom : httpIoms) { ioTargets.push_back(iom.get()); }

    auto httpserver = std::make_shared<sylar::HttpServer>(httpIoms[0].get(), httpIoms[0].get());
    setup_http_server(httpserver, redis);
    httpserver->setIoManagers(ioTargets);
    httpIoms[0]->schedule(run_http_instance(httpIoms[0].get(), httpserver));

    // TCP服务器：单独一个实例
    auto tcpIom = std::make_shared<sylar::IOManager>(1, false);
    tcpIom->schedule(run(tcpIom.get()));

    // 打印连接信息
    printServerConnectInfo(g_http_port);

    // 等待服务器停止
    SYLAR_LOG_INFO(sylar::g_logger) << "HTTP服务器正在运行（" << kHttpInstances
                                    << " 个实例），按Ctrl+C停止";

    // 主线程循环等待退出请求，收到后再在主线程安全地执行关停
    while (!g_stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    SYLAR_LOG_INFO(sylar::g_logger) << "收到停止请求，开始关停...";
    for (auto& iom : httpIoms) {
        iom->stop();
    }
    if (tcpIom) {
        tcpIom->stop();
    }

    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";
    SYLAR_LOG_INFO(sylar::g_logger) << "HTTP服务器已退出";
    SYLAR_LOG_INFO(sylar::g_logger) << "========================================";

    return 0;
}
