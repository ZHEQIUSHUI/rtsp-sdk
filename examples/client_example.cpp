/**
 * RTSP Client 示例
 * 
 * 演示如何使用 RTSP Client 库拉流
 */

#include <rtsp-client/rtsp_client.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>

using namespace rtsp;

static std::atomic<bool> g_running(true);

void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <rtsp_url>" << std::endl;
        std::cout << "       " << argv[0] << " <rtsp_url> [--duration <sec>] [--log-format plain|json] [--prefer-tcp]" << std::endl;
        std::cout << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  " << argv[0] << " rtsp://127.0.0.1:8554/live/stream" << std::endl;
        return 1;
    }

    std::string url = argv[1];
    uint32_t run_duration_sec = 0;
    RtspClientConfig client_config;
    LogConfig log_cfg;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) {
            run_duration_sec = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--prefer-tcp") {
            client_config.prefer_tcp_transport = true;
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
        } else {
            std::cerr << "Unknown arg: " << arg << std::endl;
            return 1;
        }
    }

    std::signal(SIGINT, signalHandler);
    setLogConfig(log_cfg);

    std::cout << "=== RTSP Client Example ===" << std::endl;
    std::cout << "URL: " << url << std::endl;
    if (run_duration_sec > 0) {
        std::cout << "Duration: " << run_duration_sec << "s" << std::endl;
    }
    std::cout << "TransportPreference: " << (client_config.prefer_tcp_transport ? "tcp" : "udp") << std::endl;
    std::cout << std::endl;

    // 创建客户端
    RtspClient client;
    client.setConfig(client_config);

    // 设置帧回调
    uint64_t frame_count = 0;
    client.setFrameCallback([&frame_count](const VideoFrame& frame) {
        frame_count++;
        std::cout << "\rReceived frame #" << frame_count 
                  << " " << frame.width << "x" << frame.height
                  << " pts=" << frame.pts << "ms" << std::flush;
    });

    client.setErrorCallback([](const std::string& error) {
        std::cerr << "\n[ERROR] " << error << std::endl;
    });

    // 连接服务器
    std::cout << "[1/4] Connecting to server..." << std::endl;
    if (!client.open(url)) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    std::cout << "      Connected!" << std::endl;

    // 发送DESCRIBE
    std::cout << "[2/4] Getting stream info..." << std::endl;
    if (!client.describe()) {
        std::cerr << "Failed to get stream info" << std::endl;
        return 1;
    }

    auto info = client.getSessionInfo();
    std::cout << "      Found " << info.media_streams.size() << " stream(s)" << std::endl;
    if (!info.media_streams.empty()) {
        auto& media = info.media_streams[0];
        std::cout << "      Codec: " << media.codec_name << std::endl;
        std::cout << "      Resolution: " << media.width << "x" << media.height << std::endl;
        std::cout << "      FPS: " << media.fps << std::endl;
    }

    // 发送SETUP
    std::cout << "[3/4] Setting up stream..." << std::endl;
    if (!client.setup(0)) {
        std::cerr << "Failed to setup stream" << std::endl;
        return 1;
    }
    std::cout << "      Stream ready!" << std::endl;

    // 播放
    std::cout << "[4/4] Playing..." << std::endl;
    if (!client.play(0)) {
        std::cerr << "Failed to play" << std::endl;
        return 1;
    }

    std::cout << std::endl;
    std::cout << "Receiving frames. Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;

    const auto start_tp = std::chrono::steady_clock::now();
    // 接收循环
    while (g_running && client.isPlaying()) {
        if (run_duration_sec > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_tp).count();
            if (elapsed >= static_cast<int64_t>(run_duration_sec)) {
                break;
            }
        }
        VideoFrame frame;
        if (client.receiveFrame(frame, 1000)) {
            // 处理帧（这里只是打印信息，实际应用可以解码显示）
            delete[] frame.data;
        }
    }

    std::cout << "\n\nStopping..." << std::endl;
    client.teardown();
    RtspClientStats stats = client.getStats();
    client.close();

    std::cout << "Total frames received: " << frame_count << std::endl;
    std::cout << "CLIENT_STATS auth_retries=" << stats.auth_retries
              << " rtp_packets_received=" << stats.rtp_packets_received
              << " rtp_packets_reordered=" << stats.rtp_packets_reordered
              << " rtp_packet_loss_events=" << stats.rtp_packet_loss_events
              << " frames_output=" << stats.frames_output
              << " using_tcp_transport=" << (stats.using_tcp_transport ? 1 : 0)
              << std::endl;

    return 0;
}
