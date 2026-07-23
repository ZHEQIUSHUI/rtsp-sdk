// rtsp-cli push —— 把本地视频推到 RTSP 或 RTMP。
//   rtsp:// 目标端口无服务器：自起 RtspServer 托管（消费者来拉）。
//   rtsp:// 目标端口已有服务器：用 RtspPublisher ANNOUNCE/RECORD 推给它。
//   rtmp://：用 RtmpPublisher 推给该 RTMP 服务器/CDN（不自起；RTMP 无法本地托管）。
//   -r 控制循环次数（-1 无限 / 0|1 一次 / N 次）。

#include "cli.h"
#include "media_file.h"

#include <rtsp-server/rtsp-server.h>
#include <rtsp-publisher/rtsp-publisher.h>
#include <rtsp-rtmp/rtmp_publisher.h>
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

struct RtspTarget { std::string host; uint16_t port = 554; std::string path = "/"; };

bool parseRtspTarget(const std::string& url, RtspTarget& t) {
    const std::string scheme = "rtsp://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    std::string rest = url.substr(scheme.size());
    const size_t at = rest.find('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    const size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    t.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        t.host = authority.substr(0, colon);
        uint32_t p = 0;
        if (!parseUint32Safe(authority.substr(colon + 1), p) || p == 0 || p > 65535) return false;
        t.port = (uint16_t)p;
    } else { t.host = authority; t.port = 554; }
    return !t.host.empty();
}

bool hasListener(const std::string& host, uint16_t port) {
    Socket s; bool ok = s.connect(host, port, 600); s.close(); return ok;
}

// 统一的帧出口：自起 server / rtsp publisher / rtmp publisher 三选一。
struct Sink {
    RtspServer* server = nullptr;
    std::string path;
    RtspPublisher* pub = nullptr;
    RtmpPublisher* rtmp = nullptr;
    CodecType codec = CodecType::H264;

    bool push(const AccessUnit& au) {
        if (server) {
            return codec == CodecType::H264
                ? server->pushH264Data(path, au.data.data(), au.data.size(), au.pts_ms, au.is_key)
                : server->pushH265Data(path, au.data.data(), au.data.size(), au.pts_ms, au.is_key);
        }
        if (rtmp) {
            return codec == CodecType::H264
                ? rtmp->pushH264Data(au.data.data(), au.data.size(), au.pts_ms, au.is_key)
                : rtmp->pushH265Data(au.data.data(), au.data.size(), au.pts_ms, au.is_key);
        }
        if (pub) {
            VideoFrame f{};
            f.codec = codec; f.type = au.is_key ? FrameType::IDR : FrameType::P;
            f.data = const_cast<uint8_t*>(au.data.data()); f.size = au.data.size();
            f.pts = au.pts_ms; f.dts = au.pts_ms;
            return pub->pushFrame(f);
        }
        return false;
    }
};

} // namespace

int run_push(const PushOpts& o) {
    const bool is_rtmp = o.output.compare(0, 7, "rtmp://") == 0;
    const bool is_rtsp = o.output.compare(0, 7, "rtsp://") == 0;
    if (!is_rtmp && !is_rtsp) {
        std::fprintf(stderr, "错误: --output 必须是 rtsp:// 或 rtmp://\n");
        return 2;
    }
    RtspTarget tgt;
    if (is_rtsp && !parseRtspTarget(o.output, tgt)) {
        std::fprintf(stderr, "错误: --output 必须是 rtsp://host[:port]/path\n");
        return 2;
    }

    std::string err;
    auto reader = MediaReader::open(o.input, o.fps, err);
    if (!reader) { std::fprintf(stderr, "错误: 打开输入失败: %s\n", err.c_str()); return 1; }
    const MediaParams pm = reader->params();
    const int fps = pm.fps > 0 ? pm.fps : (o.fps > 0 ? o.fps : 25);

    const bool listener = is_rtsp && hasListener(tgt.host, tgt.port);
    const int total_loops = (o.loop < 0) ? -1 : (o.loop <= 1 ? 1 : o.loop);
    const char* mode = is_rtmp ? "RTMP publish (to server/CDN)"
                               : (listener ? "publish to existing server (ANNOUNCE/RECORD)"
                                           : "self-hosted server");

    // ---- 信息块 ----
    std::fprintf(stderr,
        "=== Input Info ===\n"
        "  File:       %s\n  Codec:      %s\n  Resolution: %dx%d\n  FPS:        %d\n"
        "  Frames:     %llu\n  SPS/PPS:    %zu / %zu bytes\n"
        "=== Target ===\n  URL:        %s\n  Mode:       %s\n  Loop:       %s\n==============\n",
        o.input.c_str(), codecName(pm.codec), pm.width, pm.height, fps,
        (unsigned long long)reader->totalUnits(), pm.sps.size(), pm.pps.size(),
        o.output.c_str(), mode,
        total_loops < 0 ? "infinite" : (total_loops == 1 ? "once" : std::to_string(total_loops).c_str()));

    // ---- 建立 sink ----
    std::unique_ptr<RtspServer> server;
    std::unique_ptr<RtspPublisher> pub;
    std::unique_ptr<RtmpPublisher> rtmp;
    Sink sink; sink.codec = pm.codec; sink.path = tgt.path;

    if (is_rtmp) {
        rtmp.reset(new RtmpPublisher());
        RtmpPublishMediaInfo media;
        media.codec = pm.codec;
        media.width = (uint32_t)(pm.width > 0 ? pm.width : 1920);
        media.height = (uint32_t)(pm.height > 0 ? pm.height : 1080);
        media.fps = (uint32_t)fps;
        media.sps = pm.sps; media.pps = pm.pps; media.vps = pm.vps;
        if (!rtmp->open(o.output, media)) {
            // RTMP 不像 RTSP 能本地自起：连不上目标服务器就直接失败。
            std::fprintf(stderr, "错误: 无法连接 RTMP 服务器（RTMP 不会自起服务器，请确保目标在线）: %s\n",
                         rtmp->getLastError().c_str());
            return 1;
        }
        sink.rtmp = rtmp.get();
        std::fprintf(stderr, "已连接 RTMP，开始推送（Ctrl-C 停止）\n");
    } else if (!listener) {
        server.reset(new RtspServer());
        if (!server->init("0.0.0.0", tgt.port)) {
            std::fprintf(stderr, "错误: 端口 %u 被占用，无法自起 RTSP 服务器（端口冲突）\n", tgt.port); return 1;
        }
        PathConfig pc;
        pc.path = tgt.path; pc.codec = pm.codec;
        pc.width = (uint32_t)(pm.width > 0 ? pm.width : 0);
        pc.height = (uint32_t)(pm.height > 0 ? pm.height : 0);
        pc.fps = (uint32_t)fps; pc.sps = pm.sps; pc.pps = pm.pps; pc.vps = pm.vps;
        if (!server->addPath(pc)) { std::fprintf(stderr, "错误: addPath 失败\n"); return 1; }
        if (!server->start()) {
            std::fprintf(stderr, "错误: 端口 %u 被占用，无法自起 RTSP 服务器（端口冲突）\n", tgt.port); return 1;
        }
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
        if (!pub->open(o.output)) { std::fprintf(stderr, "错误: 连接 %s 失败\n", o.output.c_str()); return 1; }
        PublishMediaInfo media;
        media.codec = pm.codec; media.payload_type = (pm.codec == CodecType::H264) ? 96 : 97;
        media.width = (uint32_t)(pm.width > 0 ? pm.width : 1920);
        media.height = (uint32_t)(pm.height > 0 ? pm.height : 1080);
        media.fps = (uint32_t)fps; media.sps = pm.sps; media.pps = pm.pps; media.vps = pm.vps;
        // 端口有监听但 ANNOUNCE/SETUP/RECORD 不成 → 大概率是端口冲突（对端不是可推流的
        // RTSP 服务器），或需鉴权。直接报错，不强行。
        const char* conflict = "（端口被占用但不是可推流的 RTSP 服务器，或需 --auth 鉴权）";
        if (!pub->announce(media)) { std::fprintf(stderr, "错误: 推流到 %s 失败%s\n", o.output.c_str(), conflict); return 1; }
        if (!pub->setup())  { std::fprintf(stderr, "错误: SETUP 失败%s\n", conflict); return 1; }
        if (!pub->record()) { std::fprintf(stderr, "错误: RECORD 失败%s\n", conflict); return 1; }
        sink.pub = pub.get();
        std::fprintf(stderr, "已连接，开始推送（Ctrl-C 停止）\n");
    }

    // ---- 推送循环（绝对时钟节奏）----
    const auto wall_start = clk::now();
    auto t_last = wall_start;
    double stream_ms = 0.0;
    int64_t prev_pts = -1;
    uint64_t total_frames = 0, total_bytes = 0, win_frames = 0, win_bytes = 0;
    uint64_t prev_srv_bytes = 0, prev_rtmp_bytes = 0;
    int done_loops = 0;
    bool wrote_status = false, stop = false, had_error = false;
    const double frame_ms = 1000.0 / fps;

    while (!stop && !g_interrupted.load()) {
        reader->rewind();
        AccessUnit au;
        while (!g_interrupted.load() && reader->next(au)) {
            double dt = (prev_pts < 0) ? 0.0 : (double)au.pts_ms - (double)prev_pts;
            if (prev_pts >= 0 && (dt <= 0.0 || dt > 2000.0)) dt = frame_ms;
            stream_ms += (prev_pts < 0) ? 0.0 : dt;
            prev_pts = (int64_t)au.pts_ms;
            std::this_thread::sleep_until(wall_start + std::chrono::microseconds((int64_t)(stream_ms * 1000.0)));

            // 用跨循环单调递增的 stream_ms 作为时间戳，而非会随 rewind 重置回 0 的
            // au.pts_ms：循环推流时，时间戳倒退会让 RTMP 服务器(mediamtx 等)在循环
            // 边界拒绝/重置流，导致无法建立发布。RTSP 能容忍，但统一用单调值更稳。
            au.pts_ms = (uint64_t)(stream_ms + 0.5);

            if (sink.push(au)) { ++total_frames; total_bytes += au.data.size(); ++win_frames; win_bytes += au.data.size(); }
            else if (sink.rtmp && !sink.rtmp->isConnected()) {
                std::fprintf(stderr, "\n错误: RTMP 连接断开: %s\n", sink.rtmp->getLastError().c_str());
                stop = true; had_error = true; break;
            }

            if (!o.quiet) {
                double win_sec = std::chrono::duration<double>(clk::now() - t_last).count();
                if (win_sec * 1000.0 >= o.stats_interval_ms) {
                    double fps_inst = win_sec > 0 ? win_frames / win_sec : 0.0;
                    double kbps; std::string tail;
                    if (sink.server) {
                        RtspServerStats st = sink.server->getStats();
                        kbps = win_sec > 0 ? ((st.rtp_bytes_sent - prev_srv_bytes) * 8.0 / win_sec / 1000.0) : 0.0;
                        prev_srv_bytes = st.rtp_bytes_sent;
                        tail = "clients " + std::to_string(st.sessions_created);
                    } else if (sink.rtmp) {
                        RtmpPublisher::Stats st = sink.rtmp->getStats();
                        kbps = win_sec > 0 ? ((st.bytes_sent - prev_rtmp_bytes) * 8.0 / win_sec / 1000.0) : 0.0;
                        prev_rtmp_bytes = st.bytes_sent;
                        tail = "->rtmp";
                    } else {
                        kbps = win_sec > 0 ? (win_bytes * 8.0 / win_sec / 1000.0) : 0.0;
                        tail = "->server";
                    }
                    std::fprintf(stderr,
                        "\r[push] %6.1fs | %8llu frames | %5.1f fps | %7.0f kbps | loop %d%s | %s   ",
                        secsSince(wall_start), (unsigned long long)total_frames, fps_inst, kbps,
                        done_loops + 1,
                        total_loops < 0 ? "/inf" : (std::string("/") + std::to_string(total_loops)).c_str(),
                        tail.c_str());
                    std::fflush(stderr);
                    wrote_status = true; win_frames = 0; win_bytes = 0; t_last = clk::now();
                }
            }
        }
        ++done_loops;
        if (total_loops >= 0 && done_loops >= total_loops) stop = true;
    }

    if (wrote_status) std::fprintf(stderr, "\n");
    if (rtmp) rtmp->close();
    if (pub) pub->close();
    if (server) server->stop();

    double total_sec = secsSince(wall_start);
    std::fprintf(stderr,
        "=== Summary ===\n  Duration:   %.1f s\n  Loops:      %d\n  Frames:     %llu pushed\n  Avg bitrate:%.0f kbps\n",
        total_sec, done_loops, (unsigned long long)total_frames,
        total_sec > 0 ? (total_bytes * 8.0 / total_sec / 1000.0) : 0.0);
    // 用户 Ctrl-C 停止视为正常退出；否则连接中途出错或一帧都没推出去，返回非零。
    if (g_interrupted.load()) return 0;
    if (had_error) return 1;
    if (total_frames == 0) { std::fprintf(stderr, "错误: 未推出任何帧\n"); return 1; }
    return 0;
}

} // namespace rtspcli
