#pragma once

// RTSP Client Library - 主头文件

#include <rtsp-common/common.h>
#include "rtsp_client.h"

// 版本信息
#define RTSP_CLIENT_VERSION_MAJOR 1
#define RTSP_CLIENT_VERSION_MINOR 0
#define RTSP_CLIENT_VERSION_PATCH 0

namespace rtsp {
    inline std::string getClientVersionString() {
        return "1.0.0";
    }
}
