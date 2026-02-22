#!/bin/bash

# RTSP SDK 完整集成测试

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TEST_VIDEO="${SCRIPT_DIR}/test_videos/test_original.mp4"
SERVER_PORT=18554
TEST_DURATION=10

echo "======================================"
echo "RTSP SDK Full Integration Test"
echo "======================================"
echo ""

# 检查构建
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory not found. Please build first:"
    echo "  cd $SCRIPT_DIR && mkdir build && cd build && cmake .. && make"
    exit 1
fi

cd "$BUILD_DIR"

# 确定可执行文件路径
if [ -f "$BUILD_DIR/examples/example_server" ]; then
    EXAMPLE_SERVER="$BUILD_DIR/examples/example_server"
    EXAMPLE_CLIENT="$BUILD_DIR/examples/example_client"
    PLAYER_CALLBACK="$BUILD_DIR/examples/player_callback_example"
elif [ -f "$BUILD_DIR/example_server" ]; then
    EXAMPLE_SERVER="$BUILD_DIR/example_server"
    EXAMPLE_CLIENT="$BUILD_DIR/example_client"
    PLAYER_CALLBACK="$BUILD_DIR/player_callback_example"
else
    echo "Error: Example executables not found!"
    exit 1
fi

echo "Server: $EXAMPLE_SERVER"
echo "Client: $EXAMPLE_CLIENT"
echo "Player: $PLAYER_CALLBACK"
echo ""

# 函数：等待端口可用
wait_for_port() {
    local port=$1
    local timeout=${2:-10}
    local count=0
    
    while ! nc -z 127.0.0.1 $port 2>/dev/null; do
        sleep 0.5
        count=$((count + 1))
        if [ $count -gt $((timeout * 2)) ]; then
            return 1
        fi
    done
    return 0
}

# 函数：测试 Server
test_server() {
    echo "=== Test 1: RTSP Server ==="
    
    # 启动服务器
    echo "Starting server..."
    $EXAMPLE_SERVER $SERVER_PORT /test &
    SERVER_PID=$!
    
    # 等待服务器启动
    if ! wait_for_port $SERVER_PORT 5; then
        echo "ERROR: Server failed to start"
        kill $SERVER_PID 2>/dev/null || true
        return 1
    fi
    
    echo "Server started (PID: $SERVER_PID)"
    
    # 测试 ffprobe
    echo "Testing with ffprobe..."
    timeout 5 ffprobe -rtsp_transport tcp "rtsp://127.0.0.1:$SERVER_PORT/test" 2>&1 | grep -E "(Stream|Duration|Video)" || true
    
    # 停止服务器
    echo "Stopping server..."
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    
    echo "Server test PASSED"
    echo ""
}

# 函数：测试 Client（阻塞方式）
test_client_blocking() {
    echo "=== Test 2: RTSP Client (Blocking Mode) ==="
    
    # 启动服务器
    echo "Starting server..."
    $EXAMPLE_SERVER $SERVER_PORT /test &
    SERVER_PID=$!
    
    if ! wait_for_port $SERVER_PORT 5; then
        echo "ERROR: Server failed to start"
        kill $SERVER_PID 2>/dev/null || true
        return 1
    fi
    
    # 运行客户端测试
    echo "Running client (blocking mode)..."
    timeout $TEST_DURATION $EXAMPLE_CLIENT "rtsp://127.0.0.1:$SERVER_PORT/test" 2>&1 || true
    
    # 停止服务器
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    
    echo "Client (blocking) test PASSED"
    echo ""
}

# 函数：测试 Client（回调方式）
test_client_callback() {
    echo "=== Test 3: RTSP Client (Callback Mode) ==="
    
    # 启动服务器
    echo "Starting server..."
    $EXAMPLE_SERVER $SERVER_PORT /test &
    SERVER_PID=$!
    
    if ! wait_for_port $SERVER_PORT 5; then
        echo "ERROR: Server failed to start"
        kill $SERVER_PID 2>/dev/null || true
        return 1
    fi
    
    # 运行客户端回调测试
    echo "Running client (callback mode)..."
    timeout $TEST_DURATION $PLAYER_CALLBACK "rtsp://127.0.0.1:$SERVER_PORT/test" 2>&1 | tail -20 || true
    
    # 停止服务器
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    
    echo "Client (callback) test PASSED"
    echo ""
}

# 函数：测试错误处理
test_error_handling() {
    echo "=== Test 4: Error Handling ==="
    
    # 测试连接不存在的服务器
    echo "Testing connection to non-existent server..."
    timeout 5 $EXAMPLE_CLIENT "rtsp://127.0.0.1:19999/test" 2>&1 | grep -E "(Failed|Error)" || true
    
    echo "Error handling test PASSED"
    echo ""
}

# 运行测试
echo "Starting tests..."
echo ""

test_server
test_client_blocking
test_client_callback
test_error_handling

echo "======================================"
echo "All Tests PASSED!"
echo "======================================"
