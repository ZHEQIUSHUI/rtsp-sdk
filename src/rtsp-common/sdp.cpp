#include <rtsp-common/sdp.h>
#include <rtsp-common/common.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace rtsp {

// SdpBuilder实现
SdpBuilder::SdpBuilder() : media_started_(false) {
    // 设置默认值
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    uint64_t sess_id = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
    
    setVersion();
    setOrigin("-", sess_id, sess_id, "IN", "IP4", "127.0.0.1");
    setSessionName("RTSP Stream");
    setTime();
}

SdpBuilder& SdpBuilder::setVersion(int version) {
    sdp_.str("");
    sdp_ << "v=" << version << "\r\n";
    return *this;
}

SdpBuilder& SdpBuilder::setOrigin(const std::string& username, uint64_t sess_id,
                                   uint64_t sess_version, const std::string& net_type,
                                   const std::string& addr_type, const std::string& unicast_address) {
    sdp_ << "o=" << username << " " << sess_id << " " << sess_version << " "
         << net_type << " " << addr_type << " " << unicast_address << "\r\n";
    return *this;
}

SdpBuilder& SdpBuilder::setSessionName(const std::string& name) {
    sdp_ << "s=" << name << "\r\n";
    return *this;
}

SdpBuilder& SdpBuilder::setConnection(const std::string& net_type, const std::string& addr_type,
                                       const std::string& address) {
    sdp_ << "c=" << net_type << " " << addr_type << " " << address << "\r\n";
    return *this;
}

SdpBuilder& SdpBuilder::setTime(uint64_t start_time, uint64_t stop_time) {
    sdp_ << "t=" << start_time << " " << stop_time << "\r\n";
    return *this;
}

SdpBuilder& SdpBuilder::addAttribute(const std::string& name, const std::string& value) {
    sdp_ << "a=" << name;
    if (!value.empty()) {
        sdp_ << ":" << value;
    }
    sdp_ << "\r\n";
    return *this;
}

SdpBuilder& SdpBuilder::addH264Media(const std::string& control, uint16_t port,
                                      uint8_t payload_type, uint32_t clock_rate,
                                      const std::string& sps_base64, const std::string& pps_base64,
                                      uint32_t width, uint32_t height) {
    media_started_ = true;
    
    // m=video port RTP/AVP payload_type
    sdp_ << "m=video " << port << " RTP/AVP " << (int)payload_type << "\r\n";
    
    // a=rtpmap
    sdp_ << "a=rtpmap:" << (int)payload_type << " H264/" << clock_rate << "\r\n";
    
    // a=fmtp
    sdp_ << "a=fmtp:" << (int)payload_type << " packetization-mode=1";
    if (!sps_base64.empty()) {
        sdp_ << ";sprop-parameter-sets=" << sps_base64 << "," << pps_base64;
    }
    sdp_ << "\r\n";
    
    // 尺寸信息
    sdp_ << "a=cliprect:0,0," << height << "," << width << "\r\n";
    sdp_ << "a=framesize:" << (int)payload_type << " " << width << "-" << height << "\r\n";
    
    // 控制URL
    sdp_ << "a=control:" << control << "\r\n";
    
    return *this;
}

SdpBuilder& SdpBuilder::addH265Media(const std::string& control, uint16_t port,
                                      uint8_t payload_type, uint32_t clock_rate,
                                      const std::string& vps_base64, const std::string& sps_base64,
                                      const std::string& pps_base64,
                                      uint32_t width, uint32_t height) {
    media_started_ = true;
    
    // m=video port RTP/AVP payload_type
    sdp_ << "m=video " << port << " RTP/AVP " << (int)payload_type << "\r\n";
    
    // a=rtpmap
    sdp_ << "a=rtpmap:" << (int)payload_type << " H265/" << clock_rate << "\r\n";
    
    // a=fmtp
    sdp_ << "a=fmtp:" << (int)payload_type << " ";
    
    // sprop-sps, sprop-pps, sprop-vps
    bool has_param = false;
    if (!sps_base64.empty()) {
        sdp_ << "sprop-sps=" << sps_base64;
        has_param = true;
    }
    if (!pps_base64.empty()) {
        if (has_param) sdp_ << ";";
        sdp_ << "sprop-pps=" << pps_base64;
        has_param = true;
    }
    if (!vps_base64.empty()) {
        if (has_param) sdp_ << ";";
        sdp_ << "sprop-vps=" << vps_base64;
    }
    sdp_ << "\r\n";
    
    // 尺寸信息
    sdp_ << "a=framesize:" << (int)payload_type << " " << width << "-" << height << "\r\n";
    
    // 控制URL
    sdp_ << "a=control:" << control << "\r\n";
    
    return *this;
}

std::string SdpBuilder::build() const {
    return sdp_.str();
}

std::string SdpBuilder::encodeRtpmap(CodecType codec, uint8_t payload_type, uint32_t clock_rate) {
    std::ostringstream oss;
    oss << "a=rtpmap:" << (int)payload_type << " ";
    if (codec == CodecType::H264) {
        oss << "H264";
    } else {
        oss << "H265";
    }
    oss << "/" << clock_rate << "\r\n";
    return oss.str();
}

std::string SdpBuilder::encodeFmtpH264(const std::string& sps, const std::string& pps) {
    std::ostringstream oss;
    oss << "a=fmtp:96 packetization-mode=1";
    if (!sps.empty()) {
        oss << ";sprop-parameter-sets=" << sps << "," << pps;
    }
    oss << "\r\n";
    return oss.str();
}

std::string SdpBuilder::encodeFmtpH265(const std::string& vps, const std::string& sps, 
                                        const std::string& pps) {
    std::ostringstream oss;
    oss << "a=fmtp:96 ";
    if (!vps.empty()) oss << "sprop-vps=" << vps << ";";
    if (!sps.empty()) oss << "sprop-sps=" << sps << ";";
    if (!pps.empty()) oss << "sprop-pps=" << pps;
    oss << "\r\n";
    return oss.str();
}

// SdpParser实现
SdpParser::SdpParser() {}

SdpParser::SdpParser(const std::string& sdp) {
    parse(sdp);
}

bool SdpParser::parse(const std::string& sdp) {
    sdp_ = sdp;
    // 简单解析，实际使用时需要完整实现
    return true;
}

bool SdpParser::hasVideo() const {
    return sdp_.find("m=video") != std::string::npos;
}

bool SdpParser::hasAudio() const {
    return sdp_.find("m=audio") != std::string::npos;
}

SdpMediaInfo SdpParser::getVideoInfo() const {
    SdpMediaInfo info;
    // 解析视频信息
    if (sdp_.find("H264") != std::string::npos || 
        sdp_.find("h264") != std::string::npos) {
        info.codec = CodecType::H264;
        info.payload_name = "H264";
    } else if (sdp_.find("H265") != std::string::npos || 
               sdp_.find("h265") != std::string::npos ||
               sdp_.find("HEVC") != std::string::npos) {
        info.codec = CodecType::H265;
        info.payload_name = "H265";
    }
    
    // 解析sprop-parameter-sets或sprop-sps/sprop-pps
    // 这里简化处理，实际应该使用正则表达式解析
    
    return info;
}

std::string SdpParser::getControlUrl(const std::string& base_url) const {
    // 查找a=control行
    size_t pos = sdp_.find("a=control:");
    if (pos != std::string::npos) {
        pos += 10;  // 跳过"a=control:"
        size_t end = sdp_.find("\r\n", pos);
        std::string control = sdp_.substr(pos, end - pos);
        
        // 如果是相对路径，拼接base_url
        if (!control.empty() && control[0] != '*') {
            if (control.find("rtsp://") == std::string::npos) {
                // 相对路径
                if (!base_url.empty() && base_url.back() != '/' && control[0] != '/') {
                    return base_url + "/" + control;
                } else {
                    return base_url + control;
                }
            }
        }
        return control;
    }
    return base_url;
}

} // namespace rtsp
