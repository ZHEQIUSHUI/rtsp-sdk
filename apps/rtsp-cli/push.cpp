// rtsp-cli push —— 把本地视频推到 RTSP。
//   - 目标端口无服务器：自起 RtspServer 托管（消费者来拉）。
//   - 目标端口已有服务器：用 RtspPublisher ANNOUNCE/RECORD 推给它。
//   - rtmp:// 直接报错。
//   - -r 控制循环次数（-1 无限 / 0|1 一次 / N 次）。

#include "cli.h"
#include "media_file.h"

#include <rtsp-server/rtsp-server.h>
#include <rtsp-publisher/rtsp-publisher.h>
#include <rtsp-common/common.h>
#include <rtsp-common/socket.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace rtspcli {

using namespace rtsp;
using clk = std::chrono::steady_clock;

namespace {

double secsSince(clk::time_point t0) {
    return std::chrono::duration<double>(clk::now() - t0).count();
}
const char* codecName(CodecType c) { return c == CodecType::H265 ? "H.265" : "H.264"; }

struct RtspTarget {
    std::string host;
    uint16_t port = 554;
    std::string path = "/";
};

bool parseRtspTarget(const std::string& url, RtspTarget& t) {
    const std::string scheme = "rtsp://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    std::string rest = url.substr(scheme.size());
    const size_t at = rest.find('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);  // 去 userinfo
    const size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    t.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        t.host = authority.substr(0, colon);
        uint32_t p = 0;
        if (!parseUint32Safe(authority.substr(colon + 1), p) || p == 0 || p > 65535) return false;
        t.port = (uint16_t)p;
    } else {
        t.host = authority;
        t.port = 554;
    }
    return !t.host.empty();
}

// 探测目标端口是否已有 TCP 服务在监听。
bool hasListener(const std::string& host, uint16_t port) {
    Socket s;
    bool ok = s.connect(host, port, 600);
    s.close();
    return ok;
}

// 把一帧推出去（自起 server 或 publisher）。
struct Sink {
    RtspServer* server = nullptr;      // 模式 A
    std::string path;
    RtspPublisher* pub = nullptr;      // 模式 B
    CodecType codec = CodecType::H264;

    bool push(const AccessUnit& au) {
        VideoFrame f{};
        f.codec = codec;
        f.type = au.is_key ? FrameType::IDR : FrameType::P;
        f.data = const_cast<uint8_t*>(au.data.data());
        f.size = au.data.size();
        f.pts = au.pts_ms; f.dts = au.pts_ms;
        if (server) {
            return codec == CodecType::H264
                ? server->pushH264Data(path, au.data.data(), au.data.size(), au.pts_ms, au.is_key)
                : server->pushH265Data(path, au.data.data(), au.data.size(), au.pts_ms, au.is_key);
        }
        if (pub) return pub->pushFrame(f);
        return false;
    }
};

} // namespace

int run_push(const PushOpts& o) {
    if (o.output.compare(0, 7, "rtmp://") == 0) {
        std::fprintf(stderr, "错误: push 暂不支持 rtmp://（仅 rtsp://）\n");
        return 2;
    }
    RtspTarget tgt;
    if (!parseRtspTarget(o.output, tgt)) {
        std::fprintf(stderr, "错误: --output 必须是 rtsp://host[:port]/path\n");
        return 2;
    }

    // 打开输入
    std::string err;
    auto reader = MediaReader::open(o.input, o.fps, err);
    if (!reader) { std::fprintf(stderr, "错误: 打开输入失败: %s\n", err.c_str()); return 1; }
    const MediaParams pm = reader->params();
    const int fps = pm.fps > 0 ? pm.fps : (o.fps > 0 ? o.fps : 25);

    const bool listener = hasListener(tgt.host, tgt.port);
    const int total_loops = (o.loop < 0) ? -1 : (o.loop <= 1 ? 1 : o.loop);

    // ---- 流信息块 ----
    std::fprintf(stderr,
        "=== Input Info ===\n"
        "  File:       %s\n"
        "  Codec:      %s\n"
        "  Resolution: %dx%d\n"
        "  FPS:        %d\n"
        "  Frames:     %llu\n"
        "  SPS/PPS:    %zu / %zu bytes\n",
        o.input.c_str(), codecName(pm.codec), pm.width, pm.height, fps,
        (unsigned long long)reader->totalUnits(), pm.sps.size(), pm.pps.size());
    std::fprintf(stderr,
        "=== Target ===\n"
        "  URL:        %s\n"
        "  Mode:       %s\n"
        "  Loop:       %s\n"
        "==============\n",
        o.output.c_str(),
        listener ? "publish to existing server (ANNOUNCE/RECORD)" : "self-hosted server",
        total_loops < 0 ? "infinite" : (total_loops == 1 ? "once" : std::to_string(total_loops).c_str()));

    // ---- 建立 sink ----
    std::unique_ptr<RtspServer> server;
    std::unique_ptr<RtspPublisher> pub;
    Sink sink;
    sink.codec = pm.codec;
    sink.path = tgt.path;

    if (!listener) {
        server.reset(new RtspServer());
        if (!server->init("0.0.0.0", tgt.port)) {
            std::fprintf(stderr, "错误: 无法在端口 %u 启动服务器\n", tgt.port); return 1;
        }
        PathConfig pc;
        pc.path = tgt.path; pc.codec = pm.codec;
        pc.width = (uint32_t)(pm.width > 0 ? pm.width : 0);
        pc.height = (uint32_t)(pm.height > 0 ? pm.height : 0);
        pc.fps = (uint32_t)fps;
        pc.sps = pm.sps; pc.pps = pm.pps; pc.vps = pm.vps;
        if (!server->addPath(pc)) { std::fprintf(stderr, "错误: addPath 失败\n"); return 1; }
        if (!server->start()) { std::fprintf(stderr, "错误: 服务器启动失败\n"); return 1; }
        sink.server = server.get();
        std::fprintf(stderr, "已在 rtsp://%s:%u%s 托管，等待/推送中（Ctrl-C 停止）\n",
                     tgt.host.c_str(), tgt.port, tgt.path.c_str());
    } else {
        pub.reset(new RtspPublisher());
        RtspPublishConfig pcfg;
        if (!o.auth.empty()) {
            const size_t c = o.auth.find(':');
            pcfg.username = (c == std::string::npos) ? o.auth : o.auth.substr(0, c);
            pcfg.password = (c == std::string::npos) ? "" : o.auth.substr(c + 1);
        }
        pub->setConfig(pcfg);
        if (!pub->open(o.output)) { std::fprintf(stderr, "错误: 连接服务器失败\n"); return 1; }
        PublishMediaInfo media;
        media.codec = pm.codec; media.payload_type = (pm.codec == CodecType::H264) ? 96 : 97;
        media.width = (uint32_t)(pm.width > 0 ? pm.width : 1920);
        media.height = (uint32_t)(pm.height > 0 ? pm.height : 1080);
        media.fps = (uint32_t)fps;
        media.sps = pm.sps; media.pps = pm.pps; media.vps = pm.vps;
        if (!pub->announce(media)) { std::fprintf(stderr, "错误: ANNOUNCE 失败（服务器可能不接受推流或需鉴权）\n"); return 1; }
        if (!pub->setup())  { std::fprintf(stderr, "错误: SETUP 失败\n"); return 1; }
        if (!pub->record()) { std::fprintf(stderr, "错误: RECORD 失败\n"); return 1; }
        sink.pub = pub.get();
        std::fprintf(stderr, "已连接，开始推送（Ctrl-C 停止）\n");
    }

    // ---- 推送循环（绝对时钟节奏）----
    const auto wall_start = clk::now();
    auto t_last = wall_start;
    double stream_ms = 0.0;
    int64_t prev_pts = -1;
    uint64_t total_frames = 0, total_bytes = 0;
    uint64_t win_frames = 0, win_bytes = 0;
    uint64_t prev_srv_bytes = 0;
    int done_loops = 0;
    bool wrote_status = false;
    const double frame_ms = 1000.0 / fps;

    bool stop = false;
    while (!stop && !g_interrupted.load()) {
        reader->rewind();
        AccessUnit au;
        while (!g_interrupted.load() && reader->next(au)) {
            // 节奏
            double dt = (prev_pts < 0) ? 0.0 : (double)au.pts_ms - (double)prev_pts;
            if (prev_pts >= 0 && (dt <= 0.0 || dt > 2000.0)) dt = frame_ms;  // 循环回绕/大间隔
            stream_ms += (prev_pts < 0) ? 0.0 : dt;
            prev_pts = (int64_t)au.pts_ms;
            auto target = wall_start + std::chrono::microseconds((int64_t)(stream_ms * 1000.0));
            std::this_thread::sleep_until(target);

            if (sink.push(au)) {
                ++total_frames; total_bytes += au.data.size();
                ++win_frames; win_bytes += au.data.size();
            }

            if (!o.quiet) {
                double win_sec = std::chrono::duration<double>(clk::now() - t_last).count();
                if (win_sec * 1000.0 >= o.stats_interval_ms) {
                    double fps_inst = win_sec > 0 ? win_frames / win_sec : 0.0;
                    double kbps;
                    uint64_t clients = 0;
                    if (sink.server) {
                        RtspServerStats st = sink.server->getStats();
                        kbps = win_sec > 0 ? ((st.rtp_bytes_sent - prev_srv_bytes) * 8.0 / win_sec / 1000.0) : 0.0;
                        prev_srv_bytes = st.rtp_bytes_sent;
                        clients = st.sessions_created;
                    } else {
                        kbps = win_sec > 0 ? (win_bytes * 8.0 / win_sec / 1000.0) : 0.0;
                    }
                    std::fprintf(stderr,
                        "\r[push] %6.1fs | %8llu frames | %5.1f fps | %7.0f kbps | loop %d%s | clients %llu   ",
                        secsSince(wall_start), (unsigned long long)total_frames, fps_inst, kbps,
                        done_loops + 1,
                        total_loops < 0 ? "/inf" : (std::string("/") + std::to_string(total_loops)).c_str(),
                        (unsigned long long)clients);
                    std::fflush(stderr);
                    wrote_status = true;
                    win_frames = 0; win_bytes = 0; t_last = clk::now();
                }
            }
        }
        ++done_loops;
        if (total_loops >= 0 && done_loops >= total_loops) stop = true;
    }

    if (wrote_status) std::fprintf(stderr, "\n");

    if (pub) pub->close();
    if (server) server->stop();

    double total_sec = secsSince(wall_start);
    std::fprintf(stderr,
        "=== Summary ===\n"
        "  Duration:   %.1f s\n"
        "  Loops:      %d\n"
        "  Frames:     %llu pushed\n"
        "  Avg bitrate:%.0f kbps\n",
        total_sec, done_loops, (unsigned long long)total_frames,
        total_sec > 0 ? (total_bytes * 8.0 / total_sec / 1000.0) : 0.0);

    return 0;
}

} // namespace rtspcli
