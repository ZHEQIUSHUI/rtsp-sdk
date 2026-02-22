#pragma once

#include <rtsp-common/common.h>
#include <sstream>

namespace rtsp {

// SDP构建器
class SdpBuilder {
public:
    SdpBuilder();
    
    // 会话级别
    SdpBuilder& setVersion(int version = 0);
    SdpBuilder& setOrigin(const std::string& username, uint64_t sess_id, 
                          uint64_t sess_version, const std::string& net_type,
                          const std::string& addr_type, const std::string& unicast_address);
    SdpBuilder& setSessionName(const std::string& name);
    SdpBuilder& setConnection(const std::string& net_type, const std::string& addr_type,
                              const std::string& address);
    SdpBuilder& setTime(uint64_t start_time = 0, uint64_t stop_time = 0);
    SdpBuilder& addAttribute(const std::string& name, const std::string& value = "");
    
    // 媒体级别 - H.264
    SdpBuilder& addH264Media(const std::string& control, uint16_t port,
                             uint8_t payload_type, uint32_t clock_rate,
                             const std::string& sps_base64, const std::string& pps_base64,
                             uint32_t width, uint32_t height);
    
    // 媒体级别 - H.265
    SdpBuilder& addH265Media(const std::string& control, uint16_t port,
                             uint8_t payload_type, uint32_t clock_rate,
                             const std::string& vps_base64, const std::string& sps_base64,
                             const std::string& pps_base64,
                             uint32_t width, uint32_t height);
    
    // 构建SDP字符串
    std::string build() const;

private:
    std::stringstream sdp_;
    bool media_started_;
    
    std::string encodeRtpmap(CodecType codec, uint8_t payload_type, uint32_t clock_rate);
    std::string encodeFmtpH264(const std::string& sps, const std::string& pps);
    std::string encodeFmtpH265(const std::string& vps, const std::string& sps, const std::string& pps);
};

// SDP解析器（简单版本）
class SdpParser {
public:
    SdpParser();
    explicit SdpParser(const std::string& sdp);
    
    bool parse(const std::string& sdp);
    
    // 获取媒体信息
    bool hasVideo() const;
    bool hasAudio() const;
    SdpMediaInfo getVideoInfo() const;
    
    // 获取控制URL
    std::string getControlUrl(const std::string& base_url) const;

private:
    std::string sdp_;
    std::vector<SdpMediaInfo> media_infos_;
};

} // namespace rtsp
