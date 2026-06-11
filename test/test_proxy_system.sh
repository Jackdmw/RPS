#!/bin/bash
set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

PASS=0
FAIL=0
RPS_BIN=""
BACKEND_BIN=""
BACKEND_PID=""
RPS_PID=""
CONF_FILE=""
PROXY_PORT=8000
BACKEND_PORT=9090

cleanup() {
    echo ""
    echo "=== cleaning up ==="
    [ -n "$RPS_PID" ] && kill "$RPS_PID" 2>/dev/null && wait "$RPS_PID" 2>/dev/null
    [ -n "$BACKEND_PID" ] && kill "$BACKEND_PID" 2>/dev/null && wait "$BACKEND_PID" 2>/dev/null
    [ -n "$CONF_FILE" ] && rm -f "$CONF_FILE"
}
trap cleanup EXIT

assert_contains() {
    local desc="$1" response="$2" expected="$3"
    if echo "$response" | grep -q "$expected"; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc (expected '$expected' not found)"
        echo "  response: $(echo "$response" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

# ── build ──────────────────────────────────────────────────
echo "=== building ==="

cd "$(dirname "$0")/.."

# Build RPS if needed
if [ ! -f build/RPS ] || [ src/core/rps_rbtree.c -nt build/RPS ]; then
    echo "building RPS..."
    cd build && cmake .. > /dev/null 2>&1 && make -j$(nproc) > /dev/null 2>&1
    cd ..
fi
RPS_BIN="$(pwd)/build/RPS"

# Build test backend
echo "building test backend..."
gcc -o build/test_backend test/test_backend.c -Wall 2>&1
BACKEND_BIN="$(pwd)/build/test_backend"

# ── start backend ──────────────────────────────────────────
echo ""
echo "=== starting backend on :$BACKEND_PORT ==="
$BACKEND_BIN $BACKEND_PORT &
BACKEND_PID=$!
sleep 0.5

if ! kill -0 $BACKEND_PID 2>/dev/null; then
    echo "ERROR: backend failed to start"
    exit 1
fi
echo "backend PID=$BACKEND_PID"

# ── generate config & start RPS ────────────────────────────
CONF_FILE="$(mktemp /tmp/rps_test_XXXXXX.conf)"
cat > "$CONF_FILE" << EOF
worker_processes 1;
event {
    worker_connections 64;
    use epoll;
}

http {
    server {
        listen $PROXY_PORT;
        server_name "localhost:$PROXY_PORT";
        location / {
            proxy_pass "http://127.0.0.1:$BACKEND_PORT";
        }
    }
}
EOF

echo ""
echo "=== starting RPS on :$PROXY_PORT → backend :$BACKEND_PORT ==="
$RPS_BIN -c "$CONF_FILE" > /tmp/rps_test_output.log 2>&1 &
RPS_PID=$!
sleep 1

if ! kill -0 $RPS_PID 2>/dev/null; then
    echo "ERROR: RPS failed to start"
    cat /tmp/rps_test_output.log
    exit 1
fi
echo "RPS PID=$RPS_PID"

# ── run tests ──────────────────────────────────────────────
echo ""
echo "=== running proxy tests ==="

# helper: send a raw HTTP request via bash /dev/tcp
send_request() {
    local host="$1" port="$2" request="$3"
    exec 3<>/dev/tcp/$host/$port 2>/dev/null || { echo "CONNECT_FAILED"; return; }
    printf "%s" "$request" >&3
    local resp
    while IFS= read -r -t 1 -u 3 line; do
        resp+="$line"$'\n'
    done
    exec 3>&-
    echo "$resp"
}

# Use curl if available, otherwise warn
USE_CURL=false
if command -v curl &> /dev/null; then
    USE_CURL=true
fi

run_test() {
    local desc="$1" url="$2" expected="$3"
    local response

    if $USE_CURL; then
        # --http1.0: 每次请求独立连接，避免 keepalive 与 RPS 代理模式冲突
        response=$(curl -s --max-time 3 --http1.0 "$url" 2>&1 || echo "CURL_ERROR")
    fi

    if [ -z "$response" ] || [ "$response" = "CURL_ERROR" ]; then
        echo -e "  ${RED}FAIL${NC}: $desc (curl failed or empty response)"
        FAIL=$((FAIL + 1))
        # Try raw socket with correct path
        local host="127.0.0.1"
        local path
        path=$(echo "$url" | sed 's|^http://[^/]*||')
        local raw_req="GET $path HTTP/1.0\r\nHost: localhost\r\n\r\n"
        response=$(send_request "$host" "$PROXY_PORT" "$raw_req")
    fi
    assert_contains "$desc" "$response" "$expected"
}

# Test 1: basic proxy pass
run_test "basic GET /" "http://127.0.0.1:$PROXY_PORT/" "Hello from backend"

# Test 2: 404 path
run_test "GET /nonexistent" "http://127.0.0.1:$PROXY_PORT/nonexistent" "Not found"

# Test 3: echo path
run_test "GET /echo" "http://127.0.0.1:$PROXY_PORT/echo" "ECHO"

# Test 4: multiple requests (connection reuse-ish)
echo ""
echo "  --- multiple requests ---"
for i in 1 2 3; do
    if $USE_CURL; then
        resp=$(curl -s --max-time 3 --http1.0 "http://127.0.0.1:$PROXY_PORT/" 2>&1 || echo "")
        if echo "$resp" | grep -q "Hello from backend"; then
            echo -e "  ${GREEN}PASS${NC}: request $i"
            PASS=$((PASS + 1))
        else
            echo -e "  ${RED}FAIL${NC}: request $i"
            FAIL=$((FAIL + 1))
        fi
    fi
done

# Test 5: error path
run_test "GET /error" "http://127.0.0.1:$PROXY_PORT/error" "Backend error"

# ── check RPS still alive ──────────────────────────────────
echo ""
echo "=== RPS health check ==="
if kill -0 $RPS_PID 2>/dev/null; then
    echo -e "${GREEN}RPS still running${NC}"
else
    echo -e "${RED}RPS crashed during tests!${NC}"
    cat /tmp/rps_test_output.log
    FAIL=$((FAIL + 1))
fi

# ── result ─────────────────────────────────────────────────
echo ""
echo "=== result: $PASS passed, $FAIL failed ==="
if [ $FAIL -gt 0 ]; then
    echo -e "${RED} SOME TESTS FAILED ${NC}"
    exit 1
else
    echo -e "${GREEN} ALL TESTS PASSED ${NC}"
    exit 0
fi
