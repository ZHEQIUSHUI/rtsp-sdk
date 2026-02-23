#pragma once

#include <rtsp-common/common.h>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {

struct RtspPublishConfig {
    std::string user_agent = "RtspPublisher/1.0";
    uint16_t local_rtp_port = 25000;
};

struct PublishMediaInfo {
    CodecType codec = CodecType::H264;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> vps;
    uint8_t payload_type = 96;
    std::string control_track = "streamid=0";
};

class RtspPublisher {
public:
    RtspPublisher();
    ~RtspPublisher();

    void setConfig(const RtspPublishConfig& config);
    bool open(const std::string& url);
    bool announce(const PublishMediaInfo& media);
    bool setup();
    bool record();
    bool pushFrame(const VideoFrame& frame);
    bool pushH264Data(const uint8_t* data, size_t size, uint64_t pts, bool is_key);
    bool pushH265Data(const uint8_t* data, size_t size, uint64_t pts, bool is_key);
    bool teardown();
    bool closeWithTimeout(uint32_t timeout_ms);
    void close();
    bool isConnected() const;
    bool isRecording() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 兼容命名：语义上更推荐 RtspPublisher。
using RtspPusher = RtspPublisher;

} // namespace rtsp
