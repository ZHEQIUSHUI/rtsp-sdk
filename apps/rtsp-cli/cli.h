#pragma once

// rtsp-cli — 子命令选项与运行入口（pull/push 在各自的 .cpp 中实现）。

#include <atomic>
#include <string>

namespace rtspcli {

struct PullOpts {
    std::string input;          // rtsp://host:port/path（必填）
    std::string output;         // .mp4/.h264/.h265 或 "-"(stdout)，可空
    int duration_sec = 0;       // 0 = 不限时
    std::string transport;      // ""=默认(udp+fallback) / "tcp" / "udp"
    std::string auth;           // user:pass（Digest）
    int stats_interval_ms = 500;
    bool quiet = false;         // 不刷新状态行
};

struct PushOpts {
    std::string input;          // .mp4/.h264/.h265（必填）
    std::string output;         // rtsp://host:port/path（必填）
    int loop = 1;               // -1=无限, 0|1=一次, N=N 次
    int fps = 0;                // 0 = 用流自带时基/默认；裸流可指定
    std::string auth;           // user:pass（Digest）
    int stats_interval_ms = 500;
    bool quiet = false;
};

// SIGINT 标志：被信号处理器置位，pull/push 主循环据此优雅退出。
extern std::atomic<bool> g_interrupted;

int run_pull(const PullOpts& o);
int run_push(const PushOpts& o);

} // namespace rtspcli
