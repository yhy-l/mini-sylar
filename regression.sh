#!/bin/bash
# 功能回归：改动（写路径免挂起/端口参数化/注释清理/nginx多后端）后验证核心功能
PASS=0
FAIL=0
check() {
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
        PASS=$((PASS+1))
    else
        echo "FAIL: $1 (期望=$2 实际=$3)"
        FAIL=$((FAIL+1))
    fi
}

B=http://127.0.0.1:8080

echo "=== 1. HTTP 接口 ==="
check "GET /api/hello"   "200" "$(curl -s -o /dev/null -w '%{http_code}' $B/api/hello)"
check "GET /api/time"    "200" "$(curl -s -o /dev/null -w '%{http_code}' $B/api/time)"
check "POST /api/echo"   "200" "$(curl -s -o /dev/null -w '%{http_code}' -X POST -d 'a=1' $B/api/echo)"
check "GET /text"        "200" "$(curl -s -o /dev/null -w '%{http_code}' $B/text)"
# /api/limited 不在此单项检查（60秒窗口内计数会影响结果），由第4步完整验证限流
check "GET /nope (404)"  "404" "$(curl -s -o /dev/null -w '%{http_code}' $B/nope)"

echo "=== 2. 静态文件 ==="
check "GET /"               "200" "$(curl -s -o /dev/null -w '%{http_code}' $B/)"
check "GET /about.html"     "200" "$(curl -s -o /dev/null -w '%{http_code}' $B/about.html)"
check "GET /css/style.css"  "200" "$(curl -s -o /dev/null -w '%{http_code}' $B/css/style.css)"
check "GET /js/app.js"      "200" "$(curl -s -o /dev/null -w '%{http_code}' $B/js/app.js)"

echo "=== 3. nginx 80 端口（多实例负载均衡）==="
check "GET 80 /api/hello" "200" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/api/hello)"
check "GET 80 /"          "200" "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1/)"

echo "=== 4. 限流（清空计数后连续12次，应10次200+2次429）==="
redis-cli --scan --pattern 'rate:*' | xargs -r redis-cli DEL > /dev/null
CODES=""
for i in {1..12}; do
    CODES="$CODES $(curl -s -o /dev/null -w '%{http_code}' $B/api/limited)"
done
CNT200=$(echo "$CODES" | grep -o '200' | wc -l)
CNT429=$(echo "$CODES" | grep -o '429' | wc -l)
check "限流 200 次数" "10" "$CNT200"
check "限流 429 次数" "2" "$CNT429"

echo "=== 5. Redis 统计 ==="
STATS=$(curl -s $B/api/stats)
check "/api/stats 含 total" "yes" "$(echo "$STATS" | grep -c '\"total\"' | grep -q 1 && echo yes || echo no)"

echo "=== 6. TCP 8081 ==="
# TCP 服务器只收数据打日志、不回显，所以只验证"能建立连接"
TCP_OK=$(python3 -c "import socket; s=socket.create_connection(('127.0.0.1',8081),timeout=2); s.close(); print('yes')" 2>/dev/null)
check "TCP 8081 可连接" "yes" "$TCP_OK"

echo ""
echo "========== 结果: PASS=$PASS FAIL=$FAIL =========="
