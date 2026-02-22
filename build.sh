#!/bin/bash

# RTSP Server构建脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="Release"
CLEAN=0
TEST=0

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -t|--test)
            TEST=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  -d, --debug    构建Debug版本"
            echo "  -c, --clean    清理构建目录"
            echo "  -t, --test     运行测试"
            echo "  -h, --help     显示帮助"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# 清理
if [ $CLEAN -eq 1 ]; then
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

# 创建构建目录
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "Building RTSP Server (${BUILD_TYPE})..."

# 配置
cmake .. -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
         -DBUILD_TESTS=ON \
         -DBUILD_EXAMPLES=ON

# 编译
make -j$(nproc)

echo "Build completed successfully!"
echo ""
echo "Binaries are in: ${BUILD_DIR}"
echo ""
echo "To test with the sample video:"
echo "  ./examples/rtsp_server_example ../person_test_ov2_1920x1080_30fps_gop60_4Mbps.mp4"
echo ""

# 运行测试
if [ $TEST -eq 1 ]; then
    echo "Running tests..."
    ctest -V
fi
