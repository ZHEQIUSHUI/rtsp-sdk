// rtsp-cli push —— 推流（阶段 1：桩，后续阶段实现）。

#include "cli.h"

#include <cstdio>

namespace rtspcli {

int run_push(const PushOpts& o) {
    std::fprintf(stderr,
        "push: 暂未实现 (input=%s output=%s loop=%d fps=%d)\n",
        o.input.c_str(), o.output.c_str(), o.loop, o.fps);
    return 1;
}

} // namespace rtspcli
