// rtsp-cli — RTSP 拉流/推流命令行工具（基于 rtsp-sdk）。
//
//   rtsp-cli pull -i rtsp://host/path [-o out.mp4]
//   rtsp-cli push -i in.mp4 -o rtsp://host/path [-r -1]
//   rtsp-cli help [pull|push]
//
// 本文件只负责参数解析、分组 help 与子命令分发；pull/push 的实现见 pull.cpp/push.cpp。

#include "cli.h"

#include <rtsp-common/common.h>   // rtsp::parseInt32Safe

#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

namespace rtspcli {

std::atomic<bool> g_interrupted{false};

namespace {

void onSignal(int) { g_interrupted.store(true); }

// ---- help ----------------------------------------------------------------

void printPullHelp(std::FILE* out) {
    std::fprintf(out,
        "pull:\n"
        "  -i, --input <url>          RTSP 源 (rtsp://host:port/path)            [必填]\n"
        "  -o, --output <file>        保存文件 (.mp4/.h264/.h265，- 为 stdout)   [可选]\n"
        "  -t, --duration <sec>       拉流 N 秒后自动停止\n"
        "      --transport <tcp|udp>  传输方式 (默认 udp，失败回退 tcp)\n"
        "      --auth <user:pass>     RTSP Digest 鉴权\n"
        "      --stats-interval <ms>  状态行刷新间隔 (默认 500)\n"
        "      --quiet                不刷新状态行 (CI/管道友好)\n");
}

void printPushHelp(std::FILE* out) {
    std::fprintf(out,
        "push:\n"
        "  -i, --input <file>         输入视频 (.mp4/.h264/.h265)               [必填]\n"
        "  -o, --output <url>         rtsp:// 或 rtmp:// 目标                    [必填]\n"
        "                             rtsp 无服务器则自起托管；rtmp 需对端已在线，否则报错\n"
        "  -r, --loop <N>             循环: -1 无限 / 0|1 一次 / N 次 (默认 1)\n"
        "      --fps <n>              裸流无时间戳时的帧率 (mp4 用自带时基)\n"
        "      --auth <user:pass>     推到需鉴权服务器的凭据 (Digest)\n"
        "      --stats-interval <ms>  状态行刷新间隔 (默认 500)\n"
        "      --quiet                不刷新状态行\n");
}

void printTopHelp(std::FILE* out) {
    std::fprintf(out,
        "rtsp-cli — RTSP 拉流/推流工具 (基于 rtsp-sdk)\n"
        "\n"
        "用法:\n"
        "  rtsp-cli <command> [选项]\n"
        "\n"
        "命令:\n"
        "  pull    从 RTSP 拉流，打印实时状态，可选保存到文件\n"
        "  push    把本地视频推流到 RTSP（端口无服务器则自起），可循环\n"
        "  help    显示帮助 (help <command> 看单个命令)\n"
        "\n");
    printPullHelp(out);
    std::fprintf(out, "\n");
    printPushHelp(out);
    std::fprintf(out,
        "\n"
        "help:\n"
        "  rtsp-cli help              显示本帮助\n"
        "  rtsp-cli help <command>    显示某命令的详细帮助\n"
        "\n"
        "示例:\n"
        "  rtsp-cli pull -i rtsp://127.0.0.1:8554/live -o out.mp4\n"
        "  rtsp-cli push -i in.mp4  -o rtsp://127.0.0.1:8554/live -r -1\n");
}

// ---- 参数解析 ------------------------------------------------------------

enum class Parse { Ok, Help, Error };

// 取下一个 token 作为选项值；缺失则报错。
const char* takeValue(int argc, char** argv, int& i, const std::string& opt, std::string& err) {
    if (i + 1 >= argc) { err = "缺少 " + opt + " 的值"; return nullptr; }
    return argv[++i];
}

bool parseIntOpt(const char* s, int& out, const std::string& opt, std::string& err) {
    int32_t v = 0;
    if (!rtsp::parseInt32Safe(s, v)) { err = opt + " 需要整数，得到: " + s; return false; }
    out = static_cast<int>(v);
    return true;
}

Parse parsePull(int argc, char** argv, PullOpts& o, std::string& err) {
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") return Parse::Help;
        else if (a == "-i" || a == "--input")  { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; o.input = v; }
        else if (a == "-o" || a == "--output") { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; o.output = v; }
        else if (a == "-t" || a == "--duration") { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; if (!parseIntOpt(v, o.duration_sec, a, err)) return Parse::Error; }
        else if (a == "--transport") { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; o.transport = v; }
        else if (a == "--auth")      { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; o.auth = v; }
        else if (a == "--stats-interval") { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; if (!parseIntOpt(v, o.stats_interval_ms, a, err)) return Parse::Error; }
        else if (a == "--quiet") { o.quiet = true; }
        else { err = "未知选项: " + a; return Parse::Error; }
    }
    if (o.input.empty()) { err = "pull: 缺少必填 --input"; return Parse::Error; }
    if (!o.transport.empty() && o.transport != "tcp" && o.transport != "udp") {
        err = "--transport 只能是 tcp 或 udp"; return Parse::Error;
    }
    return Parse::Ok;
}

Parse parsePush(int argc, char** argv, PushOpts& o, std::string& err) {
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") return Parse::Help;
        else if (a == "-i" || a == "--input")  { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; o.input = v; }
        else if (a == "-o" || a == "--output") { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; o.output = v; }
        else if (a == "-r" || a == "--loop")   { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; if (!parseIntOpt(v, o.loop, a, err)) return Parse::Error; }
        else if (a == "--fps")  { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; if (!parseIntOpt(v, o.fps, a, err)) return Parse::Error; }
        else if (a == "--auth") { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; o.auth = v; }
        else if (a == "--stats-interval") { auto v = takeValue(argc, argv, i, a, err); if (!v) return Parse::Error; if (!parseIntOpt(v, o.stats_interval_ms, a, err)) return Parse::Error; }
        else if (a == "--quiet") { o.quiet = true; }
        else { err = "未知选项: " + a; return Parse::Error; }
    }
    if (o.input.empty())  { err = "push: 缺少必填 --input";  return Parse::Error; }
    if (o.output.empty()) { err = "push: 缺少必填 --output"; return Parse::Error; }
    return Parse::Ok;
}

} // namespace
} // namespace rtspcli

int main(int argc, char** argv) {
    using namespace rtspcli;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (argc < 2) { printTopHelp(stderr); return 2; }

    const std::string cmd = argv[1];

    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        if (argc >= 3) {
            const std::string sub = argv[2];
            if (sub == "pull") printPullHelp(stdout);
            else if (sub == "push") printPushHelp(stdout);
            else printTopHelp(stdout);
        } else {
            printTopHelp(stdout);
        }
        return 0;
    }

    if (cmd == "pull") {
        PullOpts o; std::string err;
        const Parse r = parsePull(argc - 2, argv + 2, o, err);
        if (r == Parse::Help)  { printPullHelp(stdout); return 0; }
        if (r == Parse::Error) { std::fprintf(stderr, "错误: %s\n\n", err.c_str()); printPullHelp(stderr); return 2; }
        return run_pull(o);
    }

    if (cmd == "push") {
        PushOpts o; std::string err;
        const Parse r = parsePush(argc - 2, argv + 2, o, err);
        if (r == Parse::Help)  { printPushHelp(stdout); return 0; }
        if (r == Parse::Error) { std::fprintf(stderr, "错误: %s\n\n", err.c_str()); printPushHelp(stderr); return 2; }
        return run_push(o);
    }

    std::fprintf(stderr, "未知命令: %s\n\n", cmd.c_str());
    printTopHelp(stderr);
    return 2;
}
