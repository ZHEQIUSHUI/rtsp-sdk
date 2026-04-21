// 并发 SETUP + 推帧压力：用于 TSan 下验证 C1 锁序 / C3 paths_ / H1 端口分配原子性 / C5 config 竞争
#include <rtsp-server/rtsp-server.h>
#include <rtsp-client/rtsp-client.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace rtsp;
using namespace std::chrono_literals;

static std::vector<uint8_t> makeNalu(bool keyframe) {
    std::vector<uint8_t> out;
    // Annex-B start code + NALU header
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
    if (keyframe) {
        out.push_back(0x67);  // SPS
        out.insert(out.end(), 30, 0x42);
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.push_back(0x68);  // PPS
        out.insert(out.end(), 5, 0xCE);
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.push_back(0x65);  // IDR
    } else {
        out.push_back(0x41);  // P
    }
    out.insert(out.end(), 500, 0x80);
    return out;
}

int main() {
    const uint16_t kPort = 18661;
    auto server = std::make_shared<RtspServer>();
    server->init("127.0.0.1", kPort);
    PathConfig cfg;
    cfg.path = "/live";
    cfg.codec = CodecType::H264;
    server->addPath(cfg);
    if (!server->start()) {
        std::cerr << "SKIP concurrent setup: failed to bind" << std::endl;
        return 0;
    }
    std::this_thread::sleep_for(200ms);

    // Pusher 线程：高频推帧（触发 broadcastFrame + config auto-extract 写）
    std::atomic<bool> running{true};
    std::thread pusher([&]() {
        auto key = makeNalu(true);
        auto p   = makeNalu(false);
        uint64_t pts = 0;
        int i = 0;
        while (running.load()) {
            bool is_key = (i % 30 == 0);
            const auto& buf = is_key ? key : p;
            server->pushH264Data("/live", buf.data(), buf.size(), pts, is_key);
            pts += 33;
            ++i;
            std::this_thread::sleep_for(10ms);
        }
    });

    // 多个并发 client：DESCRIBE+SETUP+PLAY+快速关闭
    const int kClients = 6;
    std::atomic<int> done{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([&, i]() {
            for (int round = 0; round < 8; ++round) {
                RtspClient c;
                std::string url = "rtsp://127.0.0.1:" + std::to_string(kPort) + "/live";
                if (!c.open(url)) continue;
                if (!c.describe()) { c.closeWithTimeout(500); continue; }
                if (!c.setup(0))   { c.closeWithTimeout(500); continue; }
                c.play(0);
                std::this_thread::sleep_for(30ms);
                c.closeWithTimeout(1000);
            }
            ++done;
        });
    }

    for (auto& t : clients) t.join();
    running = false;
    pusher.join();

    // 验证 stats 非 0 即可（真正的回归意义是 TSan 下跑不出竞争）
    auto st = server->getStats();
    std::cout << "sessions_created=" << st.sessions_created
              << " frames_pushed=" << st.frames_pushed
              << " rtp_packets_sent=" << st.rtp_packets_sent << std::endl;
    assert(st.sessions_created > 0);
    assert(st.frames_pushed > 0);

    server->stop();
    std::cout << "concurrent setup/push test passed\n";
    return 0;
}
