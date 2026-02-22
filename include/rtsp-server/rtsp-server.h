#pragma once

// RTSP Server Library - 主头文件

#include <rtsp-common/common.h>
#include "rtsp_server.h"

// 版本信息
#define RTSP_SERVER_VERSION_MAJOR 1
#define RTSP_SERVER_VERSION_MINOR 0
#define RTSP_SERVER_VERSION_PATCH 0

namespace rtsp {
    inline std::string getVersionString() {
        return "1.0.0";
    }
}
