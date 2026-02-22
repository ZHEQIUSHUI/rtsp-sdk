#!/bin/bash

# 使用ffmpeg测试RTSP服务器的脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
SERVER_BIN="${BUILD_DIR}/rtsp-server/examples/rtsp_server_example"
VIDEO_FILE="${SCRIPT_DIR}/person_test_ov2_1920x1080_30fps_gop60_4Mbps.mp4"
RTSP_URL="rtsp://127.0.0.1:8554/live/stream"

# 检查ffmpeg是否安装
if ! command -v ffmpeg &> /dev/null; then
    echo "Error: ffmpeg not found. Please install ffmpeg."
    exit 1
fi

if ! command -v ffplay &> /dev/null; then
    echo "Warning: ffplay not found. Will use ffmpeg for testing."
    FFPLAY_AVAILABLE=0
else
    FFPLAY_AVAILABLE=1
fi

# 检查视频文件
if [ ! -f "${VIDEO_FILE}" ]; then
    echo "Error: Video file not found: ${VIDEO_FILE}"
    exit 1
fi

# 检查服务器程序
if [ ! -f "${SERVER_BIN}" ]; then
    echo "Error: Server binary not found. Please build first:"
    echo "  ./build.sh"
    exit 1
fi

echo "=== RTSP Server Test ==="
echo "Video: ${VIDEO_FILE}"
echo "URL: ${RTSP_URL}"
echo ""

# 启动服务器
echo "Starting RTSP server..."
${SERVER_BIN} "${VIDEO_FILE}" 8554 /live/stream &
SERVER_PID=$!

# 等待服务器启动
sleep 2

# 检查服务器是否运行
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "Error: Server failed to start"
    exit 1
fi

echo "Server started (PID: $SERVER_PID)"
echo ""

# 测试函数
test_with_ffplay() {
    echo "Testing with ffplay..."
    timeout 10 ffplay -rtsp_transport tcp "${RTSP_URL}" -autoexit -loglevel error || true
    echo "ffplay test completed"
}

test_with_ffmpeg() {
    echo "Testing with ffmpeg..."
    OUTPUT_FILE="/tmp/rtsp_test_output.mp4"
    
    # 录制10秒
    timeout 10 ffmpeg -rtsp_transport tcp -i "${RTSP_URL}" -c copy -t 5 -y "${OUTPUT_FILE}" -loglevel error || true
    
    # 检查输出文件
    if [ -f "${OUTPUT_FILE}" ]; then
        FILE_SIZE=$(stat -c%s "${OUTPUT_FILE}" 2>/dev/null || stat -f%z "${OUTPUT_FILE}" 2>/dev/null)
        echo "Output file size: ${FILE_SIZE} bytes"
        
        if [ $FILE_SIZE -gt 10000 ]; then
            echo "✓ ffmpeg test passed!"
        else
            echo "✗ ffmpeg test failed: output file too small"
        fi
        
        rm -f "${OUTPUT_FILE}"
    else
        echo "✗ ffmpeg test failed: no output file"
    fi
}

# 运行测试
if [ $FFPLAY_AVAILABLE -eq 1 ]; then
    test_with_ffplay
fi

test_with_ffmpeg

# 停止服务器
echo ""
echo "Stopping server..."
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Test completed ==="
