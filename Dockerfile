# ---------- 构建阶段：编译 mini-sylar 与 log_consumer ----------
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-14 cmake make \
    libhiredis-dev librabbitmq-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_CXX_FLAGS="-std=c++23" \
    && cmake --build . -j$(nproc)

# ---------- 运行阶段：只保留可执行文件与静态资源 ----------
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libhiredis-dev librabbitmq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/mini-sylar /src/build/log_consumer /app/
COPY --from=build /src/web /app/web

EXPOSE 8080 8081

# 默认启动 HTTP 服务器；可用 command 覆盖，如 ["./mini-sylar", "8080", "8081"]
CMD ["./mini-sylar"]
