/**
 * SimpleRtspPlayer 回调示例
 * 
 * 演示如何使用 SimpleRtspPlayer 的回调方式接收帧
 */

#include <rtsp-client/rtsp-client.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>

using namespace rtsp;

static std::atomic<bool> g_running(true);
static std::atomic<uint64_t> g_frame_count(0);

void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <rtsp_url>" << std::endl;
        std::cout << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  " << argv[0] << " rtsp://127.0.0.1:8554/live" << std::endl;
        return 1;
    }

    std::string url = argv[1];

    std::signal(SIGINT, signalHandler);

    std::cout << "=== SimpleRtspPlayer Callback Example ===" << std::endl;
    std::cout << "URL: " << url << std::endl;
    std::cout << std::endl;

    SimpleRtspPlayer player;

    // 设置帧回调 - 在内部线程中接收帧
    player.setFrameCallback([](const VideoFrame& frame) {
        g_frame_count++;
        
        // 打印帧信息（不要在这里做耗时操作）
        std::cout << "\r[Callback] Frame #" << g_frame_count 
                  << " " << frame.width << "x" << frame.height
                  << " " << (frame.codec == CodecType::H264 ? "H.264" : "H.265")
                  << " " << (frame.type == FrameType::IDR ? "IDR" : "P")
                  << " pts=" << frame.pts << "ms"
                  << " size=" << frame.size << " bytes"
                  << std::flush;
    });

    // 设置错误回调
    player.setErrorCallback([](const std::string& error) {
        std::cerr << "\n[Error] " << error << std::endl;
    });

    // 打开并播放
    std::cout << "Connecting to: " << url << std::endl;
    if (!player.open(url)) {
        std::cerr << "Failed to open player" << std::endl;
        return 1;
    }

    // 获取媒体信息
    uint32_t width, height, fps;
    CodecType codec;
    if (player.getMediaInfo(width, height, fps, codec)) {
        std::cout << "Media Info:" << std::endl;
        std::cout << "  Resolution: " << width << "x" << height << std::endl;
        std::cout << "  FPS: " << fps << std::endl;
        std::cout << "  Codec: " << (codec == CodecType::H264 ? "H.264" : "H.265") << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Receiving frames via callback..." << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;

    // 主线程可以做其他事情，或者只是等待
    auto start_time = std::chrono::steady_clock::now();
    while (g_running && player.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // 每10秒打印一次统计
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (elapsed > 0 && elapsed % 10 == 0) {
            // 这一行会被回调的输出覆盖
        }
    }

    std::cout << "\n\nStopping..." << std::endl;
    player.close();

    std::cout << "Total frames received: " << g_frame_count << std::endl;

    return 0;
}
