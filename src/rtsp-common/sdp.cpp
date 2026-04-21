#include <rtsp-common/sdp.h>
#include <rtsp-common/common.h>
#include <chrono>
#include <iomanip>
#include <regex>
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
    media_infos_.clear();

    std::istringstream stream(sdp);
    std::string line;
    SdpMediaInfo* current_media = nullptr;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("m=video", 0) == 0) {
            media_infos_.emplace_back();
            current_media = &media_infos_.back();

            static const std::regex media_regex(R"(m=video\s+\d+\s+\S+\s+(\d+))", std::regex::icase);
            std::smatch match;
            if (std::regex_search(line, match, media_regex)) {
                uint32_t v = 0;
                if (parseUint32Safe(match[1].str(), v) && v <= 127) {
                    current_media->payload_type = static_cast<uint8_t>(v);
                }
            }
            continue;
        }

        if (current_media == nullptr) {
            continue;
        }

        if (line.rfind("a=rtpmap:", 0) == 0) {
            static const std::regex rtpmap_regex(R"(a=rtpmap:(\d+)\s+([A-Za-z0-9_-]+)/(\d+))", std::regex::icase);
            std::smatch match;
            if (std::regex_search(line, match, rtpmap_regex)) {
                uint32_t pt = 0;
                uint32_t rate = 0;
                if (!parseUint32Safe(match[1].str(), pt) || pt > 127) continue;
                if (!parseUint32Safe(match[3].str(), rate)) continue;
                current_media->payload_type = static_cast<uint8_t>(pt);
                current_media->payload_name = match[2].str();
                current_media->clock_rate = rate;

                std::string codec_name = current_media->payload_name;
                for (char& ch : codec_name) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
                if (codec_name.find("264") != std::string::npos) {
                    current_media->codec = CodecType::H264;
                } else if (codec_name.find("265") != std::string::npos ||
                           codec_name.find("HEVC") != std::string::npos) {
                    current_media->codec = CodecType::H265;
                }
            }
        } else if (line.rfind("a=framesize:", 0) == 0) {
            static const std::regex size_regex(R"(a=framesize:\d+\s+(\d+)-(\d+))", std::regex::icase);
            std::smatch match;
            if (std::regex_search(line, match, size_regex)) {
                uint32_t w = 0, h = 0;
                if (parseUint32Safe(match[1].str(), w) && parseUint32Safe(match[2].str(), h)) {
                    current_media->width = w;
                    current_media->height = h;
                }
            }
        } else if (line.rfind("a=cliprect:", 0) == 0) {
            static const std::regex clip_regex(R"(a=cliprect:\d+,\d+,(\d+),(\d+))", std::regex::icase);
            std::smatch match;
            if (std::regex_search(line, match, clip_regex)) {
                uint32_t height = 0, width = 0;
                if (parseUint32Safe(match[1].str(), height) && parseUint32Safe(match[2].str(), width)) {
                    if (current_media->width == 0)  current_media->width  = width;
                    if (current_media->height == 0) current_media->height = height;
                }
            }
        } else if (line.rfind("a=framerate:", 0) == 0) {
            static const std::regex framerate_regex(R"(a=framerate:(\d+(?:\.\d+)?))", std::regex::icase);
            std::smatch match;
            if (std::regex_search(line, match, framerate_regex)) {
                // framerate 可能是小数，先用 stod 但包 try；失败保持 0
                try {
                    double fps_d = std::stod(match[1].str());
                    if (fps_d > 0 && fps_d < 1e6) {
                        current_media->fps = static_cast<uint32_t>(fps_d);
                    }
                } catch (...) {
                    // 忽略畸形帧率
                }
            }
        } else if (line.rfind("a=fmtp:", 0) == 0) {
            static const std::regex h264_regex(R"(sprop-parameter-sets=([^,;\s]+),([^;\s]+))", std::regex::icase);
            std::smatch h264_match;
            if (std::regex_search(line, h264_match, h264_regex)) {
                current_media->sps = h264_match[1].str();
                current_media->pps = h264_match[2].str();
            }

            static const std::regex h265_vps_regex(R"(sprop-vps=([^;\s]+))", std::regex::icase);
            static const std::regex h265_sps_regex(R"(sprop-sps=([^;\s]+))", std::regex::icase);
            static const std::regex h265_pps_regex(R"(sprop-pps=([^;\s]+))", std::regex::icase);
            std::smatch match;
            if (std::regex_search(line, match, h265_vps_regex)) {
                current_media->vps = match[1].str();
            }
            if (std::regex_search(line, match, h265_sps_regex)) {
                current_media->sps = match[1].str();
            }
            if (std::regex_search(line, match, h265_pps_regex)) {
                current_media->pps = match[1].str();
            }
        }
    }

    return !media_infos_.empty();
}

bool SdpParser::hasVideo() const {
    return !media_infos_.empty();
}

bool SdpParser::hasAudio() const {
    return sdp_.find("m=audio") != std::string::npos;
}

SdpMediaInfo SdpParser::getVideoInfo() const {
    SdpMediaInfo info;
    if (!media_infos_.empty()) {
        info = media_infos_.front();
    }
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
