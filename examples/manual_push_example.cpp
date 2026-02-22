/**
 * 手动推流示例
 * 
 * 演示如何手动构造视频帧并推送到 RTSP 服务器
 */

#include <rtsp-server/rtsp-server.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <vector>

using namespace rtsp;

static std::atomic<bool> g_running(true);

void signalHandler(int) {
    g_running = false;
}

// 生成测试H.264帧
std::vector<uint8_t> generateTestFrame(uint32_t frame_index, bool is_key) {
    std::vector<uint8_t> data;
    
    if (is_key) {
        // IDR帧：包含SPS、PPS、IDR slice
        // SPS
        data.insert(data.end(), {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x28});
        // PPS
        data.insert(data.end(), {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80});
        // IDR
        data.insert(data.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x00});
    } else {
        // P帧
        data.insert(data.end(), {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A});
        data.push_back(static_cast<uint8_t>(frame_index & 0xFF));
    }
    
    return data;
}

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? std::stoi(argv[1]) : 8554;
    std::string path = (argc > 2) ? argv[2] : "/test/stream";

    std::signal(SIGINT, signalHandler);

    setLogCallback([](LogLevel level, const std::string& msg) {
        const char* level_str[] = {"[DEBUG]", "[INFO]", "[WARN]", "[ERROR]"};
        std::cout << level_str[static_cast<int>(level)] << " " << msg << std::endl;
    });

    std::cout << "=== Manual Push Example ===" << std::endl;

    // 创建服务器
    RtspServer server;
    if (!server.init("0.0.0.0", port)) {
        std::cerr << "Failed to initialize server" << std::endl;
        return 1;
    }

    // 添加路径
    PathConfig config;
    config.path = path;
    config.codec = CodecType::H264;
    config.width = 640;
    config.height = 480;
    config.fps = 30;

    if (!server.addPath(config)) {
        std::cerr << "Failed to add path" << std::endl;
        return 1;
    }

    if (!server.start()) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "Server started at rtsp://127.0.0.1:" << port << path << std::endl;

    // 生成测试帧
    uint32_t frame_index = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    while (g_running) {
        auto frame_start = std::chrono::steady_clock::now();
        
        bool is_key = (frame_index % 30 == 0);  // 每秒一个关键帧
        auto frame_data = generateTestFrame(frame_index++, is_key);
        
        uint64_t pts = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_start - start_time).count();
        
        // 推送帧
        server.pushH264Data(path, frame_data.data(), frame_data.size(), pts, is_key);
        
        // 控制帧率 (30fps)
        auto next_frame = frame_start + std::chrono::milliseconds(33);
        std::this_thread::sleep_until(next_frame);
    }

    std::cout << "\nStopping..." << std::endl;
    server.stop();

    return 0;
}
