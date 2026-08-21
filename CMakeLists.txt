cmake_minimum_required(VERSION 3.8)

#https://github.com/Kitware/CMake/blob/v4.1.1/Help/dev/experimental.rst

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
set(CMAKE_CXX_MODULE_STD ON)

project(mini-sylar LANGUAGES CXX)

add_executable(mini-sylar
    main.cpp
    log.h log.cpp
    async_log_appender.h
    mq_log_sender.h mq_log_sender.cpp
    redis_client.h redis_client.cpp
    task.h
    scheduler.h scheduler.cpp
    iomanager.h iomanager.cpp
    tcpserver.h tcpserver.cpp
    address.h address.cpp
    socket.h socket.cpp
    http/http_request.h
    http/http_parser.h
    http/http_server.h
    getFileDate.h


)


# std::thread 需要链接pthread；mq_log_sender 需要链接rabbitmq-c；redis_client 需要链接hiredis
target_link_libraries(mini-sylar PRIVATE pthread rabbitmq hiredis)

target_compile_features(mini-sylar PRIVATE cxx_std_23)

# 独立日志消费者：从RabbitMQ取日志，按级别分文件/统计告警/存结构化数据
add_executable(log_consumer
    log_consumer.cpp
)
target_compile_features(log_consumer PRIVATE cxx_std_23)
target_link_libraries(log_consumer PRIVATE rabbitmq)






































# include(GNUInstallDirs)
# install(TARGETS mini-sylar
#     LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
#     RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
# )



# set(CMAKE_CXX_STANDARD_REQUIRED ON) #强制使用指定的C++标准（如 CMAKE_CXX_STANDARD 设置的版本），而不允许回退到较旧的标准。
# set(CMAKE_CXX_STANDARD 23)           # 希望使用 C++23
# set(CMAKE_CXX_EXTENSIONS OFF) #避免编译器启用如g++的GNU扩展（如 -std=gnu++23），确保使用的是标准C++23（-std=c++23）。

# target_sources( mini-sylar
#     PRIVATE
#         main.cpp
#
#     PUBLIC
#
# )

#target_compile_options( mini-sylar PRIVATE -fmodules-ts )

#target_include_directories( mini-sylar PRIVATE /usr/include )

#target_link_directories( mini-sylar PRIVATE /usr/lib )

#target_link_libraries( mini-sylar PRIVATE m )

#set_target_properties( mini-sylar PROPERTIES
            #CXX_STANDARD 20
            #CXX_EXTENSIONS OFF
            #etc..
            #)
# 设置指定的C++编译器版本是必须的，如果不设置，或者为OFF，则指定版本不可用时，会使用上一版本。
#set_target_properties(mini-sylar PROPERTIES  CXX_STANDARD_REQUIRED on)

## --------------------------------------------------------------------------
## Adding build-requirements
## 源文件列表
#target_sources( <target> PRIVATE <source-file>... )
#
#target_precompile_headers( <target> PRIVATE <header-file>... )
#
## 预处理头文件搜索路径列表，等价参数：-I
#target_include_directories( <target> PRIVATE <include-search-dir>... )
#
## 预编译宏 自动在指定的宏前添加，等价参数：-D
#target_compile_definitions( <target> PRIVATE <macro-definitions>... )
#
## 编译选项和特性
#target_compile_options( <target> PRIVATE <compiler-option>... )
#target_compile_features( <target> PRIVATE <feature>... )
#
## 链接库
#target_link_libraries( <target> PRIVATE <dependency>... )
#
## 链接选项
#target_link_options( <target> PRIVATE <linker-option>... )
#
## 链接库搜索路径，等价参数：-L
#target_link_directories( <target> PRIVATE <linker-search-dir>... )

#execute_process(COMMAND ln -s /opt/gcc/gcm.cache  ./)

## Modern Cmake建议围绕Target和Property进行项目配置，竭力避免出现下面变量variable的set定义。
## Variable横行是典型CMake2.8时期的风格, Modern CMake则遵循了OO方法。其中，target表示待构建的对象，
## target相关的语句用于定义来约束link、compile等相关属性的scope。
#set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)


###完整的project命令
#project( mini-sylar LANGUAGES CXX )
         # DESCRIPTION "Description of project" HOMEPAGE_URL "http://www.abc.com" VERSION 0.1.0 )
#execute_process(COMMAND g++ -std=c++20 -xc++-system-header -fmodules-ts # 如何要使用modules,该命令应该放在project命令之后
#                iostream
#                string )
