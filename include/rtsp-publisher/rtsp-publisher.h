#pragma once

#include <rtsp-common/common.h>
#include "rtsp_publisher.h"

#define RTSP_PUBLISHER_VERSION_MAJOR 1
#define RTSP_PUBLISHER_VERSION_MINOR 0
#define RTSP_PUBLISHER_VERSION_PATCH 0

namespace rtsp {
inline std::string getPublisherVersionString() {
    return "1.0.0";
}
} // namespace rtsp
