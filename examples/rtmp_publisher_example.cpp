// 把本地测试帧推到外部 RTMP server（B 站 / 抖音 / 快手 / mediamtx / SRS /
// nginx-rtmp / YouTube RTMP Ingest）。
//
// 用法：
//   rtmp_publisher_example <rtmp://host[:port]/app/streamkey> [--h265] [--h265-legacy]
//
// 建议先用 mediamtx 本地试：
//   docker run -p 1935:1935 bluenviron/mediamtx:latest
//   ./rtmp_publisher_example rtmp://127.0.0.1:1935/live/test
//   ffplay rtmp://127.0.0.1:1935/live/test

#include <rtsp-rtmp/rtsp-rtmp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace rtsp;

static const uint8_t EXAMPLE_SPS[] = {0x67,0x42,0xC0,0x1F,0xD9,0x00,0x78,0x02,0x27,0xE5,
                                      0xC0,0x44,0x00,0x00,0x03,0x00,0x04,0x00,0x00,0x03,
                                      0x00,0xF0,0x3C,0x60,0xC6,0x58};
static const uint8_t EXAMPLE_PPS[] = {0x68,0xCE,0x3C,0x80};

static std::atomic<bool> g_running{true};
static void onSig(int) { g_running = false; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <rtmp://host[:port]/app/streamkey> [--h265] [--h265-legacy]"
                  << std::endl;
        return 1;
    }
    std::string url = argv[1];
    bool use_h265 = false;
    int  h265_mode = 0;   // 0 = Enhanced RTMP, 1 = legacy codecID=12
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--h265") use_h265 = true;
        else if (a == "--h265-legacy") { use_h265 = true; h265_mode = 1; }
    }

    std::signal(SIGINT, onSig);

    RtmpPublisher pub;
    RtmpPublishConfig cfg;
    cfg.out_chunk_size = 4096;
    cfg.h265_mode = h265_mode;
    pub.setConfig(cfg);

    RtmpPublishMediaInfo media;
    media.codec = use_h265 ? CodecType::H265 : CodecType::H264;
    media.width = 640;
    media.height = 480;
    media.fps = 30;
    media.bitrate_kbps = 2000;
    if (!use_h265) {
        media.sps.assign(EXAMPLE_SPS, EXAMPLE_SPS + sizeof(EXAMPLE_SPS));
        media.pps.assign(EXAMPLE_PPS, EXAMPLE_PPS + sizeof(EXAMPLE_PPS));
    }

    if (!pub.open(url, media)) {
        std::cerr << "FAIL: " << pub.getLastError() << std::endl;
        return 1;
    }
    std::cout << "Connected and publishing to " << url << std::endl;

    // 用 SPS/PPS/IDR 循环做测试流；实际使用应接你的编码器输出
    std::vector<uint8_t> key_frame;
    key_frame.insert(key_frame.end(), {0x00, 0x00, 0x00, 0x01});
    key_frame.insert(key_frame.end(), EXAMPLE_SPS, EXAMPLE_SPS + sizeof(EXAMPLE_SPS));
    key_frame.insert(key_frame.end(), {0x00, 0x00, 0x00, 0x01});
    key_frame.insert(key_frame.end(), EXAMPLE_PPS, EXAMPLE_PPS + sizeof(EXAMPLE_PPS));
    key_frame.insert(key_frame.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x00});
    key_frame.insert(key_frame.end(), 200, 0xAA);

    std::vector<uint8_t> p_frame = {0x00, 0x00, 0x00, 0x01, 0x41};
    p_frame.insert(p_frame.end(), 100, 0x55);

    uint64_t pts = 0;
    int i = 0;
    while (g_running.load()) {
        const bool is_key = (i % 30 == 0);
        const auto& f = is_key ? key_frame : p_frame;
        if (use_h265) {
            pub.pushH265Data(f.data(), f.size(), pts, is_key);
        } else {
            pub.pushH264Data(f.data(), f.size(), pts, is_key);
        }
        pts += 33;
        ++i;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    const auto st = pub.getStats();
    std::cout << "\nSent: frames=" << st.video_frames_sent
              << " messages=" << st.messages_sent
              << " bytes=" << st.bytes_sent << std::endl;
    pub.closeWithTimeout(1500);
    return 0;
}
