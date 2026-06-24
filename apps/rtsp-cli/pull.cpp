// rtsp-cli pull —— 拉流（阶段 1：桩，后续阶段实现）。

#include "cli.h"

#include <cstdio>

namespace rtspcli {

int run_pull(const PullOpts& o) {
    std::fprintf(stderr,
        "pull: 暂未实现 (input=%s output=%s duration=%d transport=%s)\n",
        o.input.c_str(), o.output.c_str(), o.duration_sec,
        o.transport.empty() ? "default" : o.transport.c_str());
    return 1;
}

} // namespace rtspcli
