/**
 * RTSP Server 示例
 * 
 * 演示如何创建 RTSP 服务器并手动推流
 */

#include <rtsp-server/rtsp-server.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace rtsp;

// 示例 H.264 SPS/PPS (Baseline Profile, 640x480)
// 实际应用中应该从编码器获取
static const uint8_t EXAMPLE_SPS[] = {0x67, 0x42, 0xC0, 0x1F, 0xD9, 0x00, 0x78, 0x02, 0x27, 0xE5, 0xC0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xF0, 0x3C, 0x60, 0xC6, 0x58};
static const uint8_t EXAMPLE_PPS[] = {0x68, 0xCE, 0x3C, 0x80};

static std::atomic<bool> g_running(true);

void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    uint16_t port = 8554;
    std::string path = "/live/stream";
    std::string auth_user;
    std::string auth_pass;
    bool use_digest_auth = false;
    LogConfig log_cfg;

    int positional_index = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--auth" && i + 1 < argc) {
            std::string auth = argv[++i];
            size_t pos = auth.find(':');
            if (pos == std::string::npos) {
                std::cerr << "Invalid --auth format, expected user:pass" << std::endl;
                return 1;
            }
            auth_user = auth.substr(0, pos);
            auth_pass = auth.substr(pos + 1);
        } else if (arg == "--digest") {
            use_digest_auth = true;
        } else if (arg == "--log-format" && i + 1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "json") {
                log_cfg.format = LogFormat::Json;
                log_cfg.use_utc_time = true;
            } else if (fmt == "plain") {
                log_cfg.format = LogFormat::PlainText;
            } else {
                std::cerr << "Unsupported --log-format: " << fmt << std::endl;
                return 1;
            }
        } else if (arg == "--log-level" && i + 1 < argc) {
            std::string level = argv[++i];
            if (level == "debug") log_cfg.min_level = LogLevel::Debug;
            else if (level == "info") log_cfg.min_level = LogLevel::Info;
            else if (level == "warn") log_cfg.min_level = LogLevel::Warning;
            else if (level == "error") log_cfg.min_level = LogLevel::Error;
            else {
                std::cerr << "Unsupported --log-level: " << level << std::endl;
                return 1;
            }
        } else if (!arg.empty() && arg[0] != '-') {
            if (positional_index == 0) {
                port = static_cast<uint16_t>(std::stoi(arg));
            } else if (positional_index == 1) {
                path = arg;
            } else {
                std::cerr << "Unexpected positional arg: " << arg << std::endl;
                return 1;
            }
            positional_index++;
        } else {
            std::cerr << "Unknown arg: " << arg << std::endl;
            return 1;
        }
    }

    std::signal(SIGINT, signalHandler);
    setLogConfig(log_cfg);

    std::cout << "=== RTSP Server Example ===" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "Path: " << path << std::endl;
    std::cout << "URL: rtsp://127.0.0.1:" << port << path << std::endl;
    std::cout << "LogFormat: " << (log_cfg.format == LogFormat::Json ? "json" : "plain") << std::endl;
    if (!auth_user.empty()) {
        std::cout << "Auth: " << (use_digest_auth ? "digest" : "basic") << " user=" << auth_user << std::endl;
    }

    // 创建服务器
    RtspServer server;
    if (!server.init("0.0.0.0", port)) {
        std::cerr << "Failed to initialize server" << std::endl;
        return 1;
    }

    // 配置路径
    PathConfig config;
    config.path = path;
    config.codec = CodecType::H264;
    config.width = 640;   // 与 SPS/PPS 匹配
    config.height = 480;
    config.fps = 30;
    
    // 可选：手动设置 SPS/PPS。
    // 如不设置，服务端会在 pushH264Data 的关键帧中自动提取并填充到 SDP。
    config.sps.assign(EXAMPLE_SPS, EXAMPLE_SPS + sizeof(EXAMPLE_SPS));
    config.pps.assign(EXAMPLE_PPS, EXAMPLE_PPS + sizeof(EXAMPLE_PPS));

    if (!server.addPath(config)) {
        std::cerr << "Failed to add path" << std::endl;
        return 1;
    }
    if (!auth_user.empty()) {
        if (use_digest_auth) {
            server.setAuthDigest(auth_user, auth_pass);
        } else {
            server.setAuth(auth_user, auth_pass);
        }
    }

    if (!server.start()) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "Server started. Press Ctrl+C to stop." << std::endl;

    // 模拟推流（生成测试帧）
    uint32_t frame_count = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (g_running) {
        // 模拟30fps
        auto frame_time = std::chrono::milliseconds(frame_count * 33);
        auto target_time = start_time + frame_time;
        std::this_thread::sleep_until(target_time);

        // 创建测试帧（H.264 IDR帧，与SPS/PPS匹配）
        std::vector<uint8_t> data;
        // SPS
        data.insert(data.end(), {0x00, 0x00, 0x00, 0x01});
        data.insert(data.end(), EXAMPLE_SPS, EXAMPLE_SPS + sizeof(EXAMPLE_SPS));
        // PPS
        data.insert(data.end(), {0x00, 0x00, 0x00, 0x01});
        data.insert(data.end(), EXAMPLE_PPS, EXAMPLE_PPS + sizeof(EXAMPLE_PPS));
        // IDR slice (简化)
        data.insert(data.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x00});

        server.pushH264Data(path, data.data(), data.size(), frame_count * 33, true);
        frame_count++;
    }

    std::cout << "\nStopping..." << std::endl;
    const RtspServerStats stats = server.getStats();
    server.stop();
    std::cout << "SERVER_STATS requests_total=" << stats.requests_total
              << " auth_challenges=" << stats.auth_challenges
              << " auth_failures=" << stats.auth_failures
              << " sessions_created=" << stats.sessions_created
              << " sessions_closed=" << stats.sessions_closed
              << " frames_pushed=" << stats.frames_pushed
              << " rtp_packets_sent=" << stats.rtp_packets_sent
              << " rtp_bytes_sent=" << stats.rtp_bytes_sent
              << std::endl;

    return 0;
}
