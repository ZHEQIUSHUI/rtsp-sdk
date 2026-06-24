// rtsp-cli pull —— 从 RTSP 拉流，打印流信息 + 实时刷新状态行，可选保存到文件。

#include "cli.h"
#include "media_file.h"

#include <rtsp-client/rtsp-client.h>
#include <rtsp-common/common.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace rtspcli {

using namespace rtsp;
using clk = std::chrono::steady_clock;

namespace {

double secsSince(clk::time_point t0) {
    return std::chrono::duration<double>(clk::now() - t0).count();
}

const char* codecName(CodecType c) { return c == CodecType::H265 ? "H.265" : "H.264"; }

// 把 user:pass 注入 rtsp URL（rtsp://host/.. -> rtsp://user:pass@host/..）
std::string injectAuth(const std::string& url, const std::string& auth) {
    if (auth.empty()) return url;
    const std::string scheme = "rtsp://";
    if (url.compare(0, scheme.size(), scheme) != 0) return url;
    if (url.find('@') != std::string::npos) return url;  // 已带凭据
    return scheme + auth + "@" + url.substr(scheme.size());
}

} // namespace

int run_pull(const PullOpts& o) {
    if (o.input.compare(0, 7, "rtsp://") != 0) {
        std::fprintf(stderr, "错误: pull 的 --input 必须是 rtsp:// 地址\n");
        return 2;
    }

    RtspClient client;
    RtspClientConfig cfg;
    if (o.transport == "tcp") { cfg.prefer_tcp_transport = true; cfg.fallback_to_tcp = true; }
    else if (o.transport == "udp") { cfg.prefer_tcp_transport = false; cfg.fallback_to_tcp = false; }
    else { cfg.prefer_tcp_transport = false; cfg.fallback_to_tcp = true; }
    client.setConfig(cfg);

    const std::string url = injectAuth(o.input, o.auth);
    if (!client.open(url)) { std::fprintf(stderr, "错误: 连接失败: %s\n", o.input.c_str()); return 1; }
    if (!client.describe()) { std::fprintf(stderr, "错误: DESCRIBE 失败\n"); client.close(); return 1; }

    SessionInfo si = client.getSessionInfo();
    if (si.media_streams.empty()) { std::fprintf(stderr, "错误: 没有可用的视频流\n"); client.close(); return 1; }
    const MediaInfo& mi = si.media_streams[0];

    if (!client.setup(0)) { std::fprintf(stderr, "错误: SETUP 失败\n"); client.close(); return 1; }
    if (!client.play(0)) { std::fprintf(stderr, "错误: PLAY 失败\n"); client.close(); return 1; }

    const bool using_tcp = client.getStats().using_tcp_transport;

    // 可选输出文件
    std::unique_ptr<MediaWriter> writer;
    if (!o.output.empty()) {
        std::string err;
        writer = MediaWriter::create(o.output, mi.codec, (int)mi.width, (int)mi.height, (int)mi.fps,
                                     mi.vps, mi.sps, mi.pps, err);
        if (!writer) { std::fprintf(stderr, "错误: 无法打开输出: %s\n", err.c_str()); client.close(); return 1; }
    }

    // ---- 一次性流信息块 ----
    std::fprintf(stderr,
        "=== Stream Info ===\n"
        "  URL:        %s\n"
        "  Transport:  %s\n"
        "  Codec:      %s\n"
        "  Resolution: %ux%u\n"
        "  FPS:        %u\n"
        "  Payload:    %u\n"
        "  SPS/PPS:    %zu / %zu bytes\n",
        o.input.c_str(), using_tcp ? "TCP (RTP over RTSP)" : "UDP",
        codecName(mi.codec), mi.width, mi.height, mi.fps, mi.payload_type,
        mi.sps.size(), mi.pps.size());
    if (mi.codec == CodecType::H265)
        std::fprintf(stderr, "  VPS:        %zu bytes\n", mi.vps.size());
    if (writer)
        std::fprintf(stderr, "  Output:     %s\n", o.output.c_str());
    std::fprintf(stderr, "===================\n");

    // ---- 接收循环 + 状态行 ----
    const auto t_start = clk::now();
    auto t_last = t_start;
    uint64_t total_frames = 0, total_bytes = 0, key_frames = 0;
    uint64_t win_frames = 0, win_bytes = 0;
    bool wrote_status = false;

    while (!g_interrupted.load()) {
        if (o.duration_sec > 0 && secsSince(t_start) >= o.duration_sec) break;
        if (!client.isConnected()) break;  // 流结束/对端断开

        VideoFrame f;
        if (client.receiveFrame(f, 200)) {
            ++total_frames; total_bytes += f.size;
            ++win_frames; win_bytes += f.size;
            if (f.type == FrameType::IDR) ++key_frames;
            if (writer && !writer->write(f)) {
                std::fprintf(stderr, "\n错误: 写文件失败\n");
                break;
            }
        }

        if (!o.quiet) {
            double win_sec = std::chrono::duration<double>(clk::now() - t_last).count();
            if (win_sec * 1000.0 >= o.stats_interval_ms) {
                RtspClientStats st = client.getStats();
                double fps = win_sec > 0 ? win_frames / win_sec : 0.0;
                double kbps = win_sec > 0 ? (win_bytes * 8.0 / win_sec / 1000.0) : 0.0;
                std::fprintf(stderr,
                    "\r[pull] %6.1fs | %8llu frames | %5.1f fps | %7.0f kbps | %6.1f MB | loss %llu | %s   ",
                    secsSince(t_start), (unsigned long long)total_frames, fps, kbps,
                    total_bytes / 1e6, (unsigned long long)st.rtp_packet_loss_events,
                    using_tcp ? "TCP" : "UDP");
                std::fflush(stderr);
                wrote_status = true;
                win_frames = 0; win_bytes = 0; t_last = clk::now();
            }
        }
    }

    if (wrote_status) std::fprintf(stderr, "\n");
    if (writer) writer->finish();
    client.close();

    // ---- 汇总 ----
    double total_sec = secsSince(t_start);
    std::fprintf(stderr,
        "=== Summary ===\n"
        "  Duration:   %.1f s\n"
        "  Frames:     %llu (%llu key)\n"
        "  Received:   %.2f MB\n"
        "  Avg bitrate:%.0f kbps\n",
        total_sec, (unsigned long long)total_frames, (unsigned long long)key_frames,
        total_bytes / 1e6,
        total_sec > 0 ? (total_bytes * 8.0 / total_sec / 1000.0) : 0.0);
    if (writer) std::fprintf(stderr, "  Saved:      %s\n", o.output.c_str());

    return (total_frames > 0 || g_interrupted.load()) ? 0 : 1;
}

} // namespace rtspcli
