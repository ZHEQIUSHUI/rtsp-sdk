#include <rtsp-server/rtsp_server.h>
#include <rtsp-common/rtsp_request.h>
#include <rtsp-common/sdp.h>
#include <rtsp-common/rtp_packer.h>
#include <rtsp-common/socket.h>
#include <rtsp-common/common.h>

#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <cstring>
#include <condition_variable>
#include <queue>
#include <vector>
#include <regex>
#include <unordered_map>
#include <future>

namespace rtsp {

namespace {

inline bool hasStartCode3(const uint8_t* data, size_t size, size_t i) {
    return i + 3 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01;
}

inline bool hasStartCode4(const uint8_t* data, size_t size, size_t i) {
    return i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 &&
           data[i + 2] == 0x00 && data[i + 3] == 0x01;
}

template <typename Fn>
void forEachAnnexBNalu(const uint8_t* data, size_t size, Fn&& fn) {
    if (!data || size == 0) {
        return;
    }

    std::vector<size_t> starts;
    starts.reserve(16);
    for (size_t i = 0; i + 3 < size; ++i) {
        if (hasStartCode4(data, size, i)) {
            starts.push_back(i + 4);
            i += 3;
        } else if (hasStartCode3(data, size, i)) {
            starts.push_back(i + 3);
            i += 2;
        }
    }

    if (starts.empty()) {
        fn(data, size);
        return;
    }

    for (size_t idx = 0; idx < starts.size(); ++idx) {
        const size_t nalu_start = starts[idx];
        size_t nalu_end = (idx + 1 < starts.size()) ? starts[idx + 1] : size;
        if (nalu_end > nalu_start) {
            fn(data + nalu_start, nalu_end - nalu_start);
        }
    }
}

bool assignIfChanged(std::vector<uint8_t>& dst, const uint8_t* src, size_t src_size) {
    if (!src || src_size == 0) {
        return false;
    }
    if (dst.size() == src_size && std::memcmp(dst.data(), src, src_size) == 0) {
        return false;
    }
    dst.assign(src, src + src_size);
    return true;
}

bool autoExtractH264ParameterSets(PathConfig& config, const uint8_t* data, size_t size) {
    bool updated = false;
    forEachAnnexBNalu(data, size, [&](const uint8_t* nalu, size_t nalu_size) {
        if (nalu_size == 0) {
            return;
        }
        const uint8_t type = nalu[0] & 0x1F;
        if (type == 7) {
            updated = assignIfChanged(config.sps, nalu, nalu_size) || updated;
        } else if (type == 8) {
            updated = assignIfChanged(config.pps, nalu, nalu_size) || updated;
        }
    });
    return updated;
}

bool autoExtractH265ParameterSets(PathConfig& config, const uint8_t* data, size_t size) {
    bool updated = false;
    forEachAnnexBNalu(data, size, [&](const uint8_t* nalu, size_t nalu_size) {
        if (nalu_size < 2) {
            return;
        }
        const uint8_t type = (nalu[0] >> 1) & 0x3F;
        if (type == 32) {
            updated = assignIfChanged(config.vps, nalu, nalu_size) || updated;
        } else if (type == 33) {
            updated = assignIfChanged(config.sps, nalu, nalu_size) || updated;
        } else if (type == 34) {
            updated = assignIfChanged(config.pps, nalu, nalu_size) || updated;
        }
    });
    return updated;
}

std::shared_ptr<std::vector<uint8_t>> makeManagedBuffer(const uint8_t* data, size_t size) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    if (data && size > 0) {
        buf->assign(data, data + size);
    }
    return buf;
}

VideoFrame cloneFrameManaged(const VideoFrame& src) {
    VideoFrame copy = src;
    copy.managed_data = makeManagedBuffer(src.data, src.size);
    copy.data = copy.managed_data->empty() ? nullptr : copy.managed_data->data();
    copy.size = copy.managed_data->size();
    return copy;
}

bool joinThreadWithTimeout(std::thread& t, uint32_t timeout_ms) {
    if (!t.joinable()) return true;
    std::thread owned = std::move(t);
    std::promise<void> done;
    auto fut = done.get_future();
    std::thread joiner([owned = std::move(owned), done = std::move(done)]() mutable {
        if (owned.joinable()) {
            owned.join();
        }
        done.set_value();
    });
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready) {
        joiner.join();
        return true;
    }
    joiner.detach();
    return false;
}

std::string toLowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool isRecordTransport(const std::string& transport) {
    return toLowerCopy(transport).find("mode=record") != std::string::npos;
}

class PublishRtpReceiver {
public:
    using FrameCallback = std::function<void(const VideoFrame&)>;

    PublishRtpReceiver() = default;
    ~PublishRtpReceiver() { stop(); }

    bool init(uint16_t rtp_port, uint16_t rtcp_port) {
        if (!rtp_socket_.bindUdp("0.0.0.0", rtp_port)) {
            return false;
        }
        if (!rtcp_socket_.bindUdp("0.0.0.0", rtcp_port)) {
            rtp_socket_.close();
            return false;
        }
        rtp_socket_.setNonBlocking(true);
        rtcp_socket_.setNonBlocking(true);
        rtp_port_ = rtp_socket_.getLocalPort();
        rtcp_port_ = rtcp_socket_.getLocalPort();
        return true;
    }

    void start() {
        if (running_) {
            return;
        }
        running_ = true;
        receive_thread_ = std::thread([this]() { receiveLoop(); });
    }

    bool stopWithTimeout(uint32_t timeout_ms) {
        if (!running_ && !receive_thread_.joinable()) {
            return true;
        }

        running_ = false;
        rtp_socket_.shutdownReadWrite();
        rtcp_socket_.shutdownReadWrite();
        if (rtp_port_ != 0) {
            Socket wake_socket;
            if (wake_socket.bindUdp("0.0.0.0", 0)) {
                uint8_t marker = 0;
                wake_socket.sendTo(&marker, 1, "127.0.0.1", rtp_port_);
            }
        }

        const bool joined = joinThreadWithTimeout(receive_thread_, timeout_ms);
        rtp_socket_.close();
        rtcp_socket_.close();
        clearCurrentFrameState();
        reorder_buffer_.clear();
        seq_initialized_ = false;
        reorder_initialized_ = false;
        return joined;
    }

    void stop() {
        (void)stopWithTimeout(2000);
    }

    void setCallback(FrameCallback callback) {
        callback_ = std::move(callback);
    }

    void setVideoInfo(CodecType codec, uint32_t width, uint32_t height, uint32_t fps, uint8_t payload_type) {
        codec_ = codec;
        width_ = width;
        height_ = height;
        fps_ = fps;
        payload_type_ = payload_type;
    }

    uint16_t getRtpPort() const { return rtp_port_; }
    uint16_t getRtcpPort() const { return rtcp_port_; }

private:
    static uint32_t parseRtpTimestampFromRaw(const uint8_t* data, size_t len) {
        if (!data || len < 8) {
            return 0;
        }
        return (static_cast<uint32_t>(data[4]) << 24) |
               (static_cast<uint32_t>(data[5]) << 16) |
               (static_cast<uint32_t>(data[6]) << 8) |
               static_cast<uint32_t>(data[7]);
    }

    static uint32_t parseRtpTimestampFromPacket(const std::vector<uint8_t>& packet) {
        return parseRtpTimestampFromRaw(packet.data(), packet.size());
    }

    void ingestRtpPacket(const uint8_t* data, size_t len) {
        if (!data || len < 12) {
            return;
        }

        const uint16_t seq = static_cast<uint16_t>((data[2] << 8) | data[3]);
        const uint32_t ts = parseRtpTimestampFromRaw(data, len);

        if (!reorder_initialized_) {
            expected_seq_ = seq;
            reorder_initialized_ = true;
        }

        reorder_buffer_[seq] = std::vector<uint8_t>(data, data + len);

        while (true) {
            auto it = reorder_buffer_.find(expected_seq_);
            if (it == reorder_buffer_.end()) {
                break;
            }
            processRtpPacket(it->second.data(), it->second.size());
            reorder_buffer_.erase(it);
            expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);
        }

        if (reorder_buffer_.size() > jitter_buffer_packets_) {
            auto it = reorder_buffer_.begin();
            expected_seq_ = it->first;
            while (true) {
                auto run = reorder_buffer_.find(expected_seq_);
                if (run == reorder_buffer_.end()) {
                    break;
                }
                processRtpPacket(run->second.data(), run->second.size());
                reorder_buffer_.erase(run);
                expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);
            }
        }

        if (!reorder_buffer_.empty() && reorder_buffer_.find(expected_seq_) == reorder_buffer_.end()) {
            auto first_it = reorder_buffer_.begin();
            const uint32_t first_ts = parseRtpTimestampFromPacket(first_it->second);
            if (ts != first_ts) {
                expected_seq_ = first_it->first;
                while (true) {
                    auto run = reorder_buffer_.find(expected_seq_);
                    if (run == reorder_buffer_.end()) {
                        break;
                    }
                    processRtpPacket(run->second.data(), run->second.size());
                    reorder_buffer_.erase(run);
                    expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);
                }
            }
        }
    }

    void appendAnnexBNalu(const uint8_t* nalu, size_t len) {
        if (!nalu || len == 0) {
            return;
        }
        static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
        frame_buffer_.insert(frame_buffer_.end(), start_code, start_code + 4);
        frame_buffer_.insert(frame_buffer_.end(), nalu, nalu + len);
    }

    bool isH265Irap(uint8_t nal_type) const {
        return nal_type >= 16 && nal_type <= 21;
    }

    void clearCurrentFrameState() {
        frame_buffer_.clear();
        frame_is_idr_ = false;
        frame_in_progress_ = false;
    }

    void emitFrame(uint32_t timestamp) {
        if (frame_buffer_.empty()) {
            return;
        }

        VideoFrame frame{};
        frame.codec = codec_;
        frame.pts = timestamp / 90;
        frame.dts = frame.pts;
        frame.width = width_;
        frame.height = height_;
        frame.fps = fps_;
        frame.type = frame_is_idr_ ? FrameType::IDR : FrameType::P;
        frame.managed_data = makeManagedBuffer(frame_buffer_.data(), frame_buffer_.size());
        frame.data = frame.managed_data->empty() ? nullptr : frame.managed_data->data();
        frame.size = frame.managed_data->size();

        if (callback_) {
            callback_(frame);
        }
        clearCurrentFrameState();
    }

    void receiveLoop() {
        uint8_t buffer[65536];
        std::string from_ip;
        uint16_t from_port = 0;

        while (running_) {
            const ssize_t len = rtp_socket_.recvFrom(buffer, sizeof(buffer), from_ip, from_port);
            if (len > 0) {
                ingestRtpPacket(buffer, static_cast<size_t>(len));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void processRtpPacket(const uint8_t* data, size_t len) {
        if (len < 12) {
            return;
        }

        const uint8_t version = (data[0] >> 6) & 0x03;
        if (version != 2) {
            return;
        }

        const bool marker = (data[1] >> 7) & 0x01;
        const uint8_t payload_type = data[1] & 0x7F;
        const uint16_t seq = static_cast<uint16_t>((data[2] << 8) | data[3]);
        const uint32_t timestamp = (static_cast<uint32_t>(data[4]) << 24) |
                                   (static_cast<uint32_t>(data[5]) << 16) |
                                   (static_cast<uint32_t>(data[6]) << 8) |
                                   static_cast<uint32_t>(data[7]);

        if (seq_initialized_) {
            const uint16_t expected = static_cast<uint16_t>(last_seq_ + 1);
            if (seq != expected && codec_ == CodecType::H265 && h265_fu_in_progress_) {
                h265_fu_drop_mode_ = true;
                h265_fu_in_progress_ = false;
                if (h265_fu_start_offset_ <= frame_buffer_.size()) {
                    frame_buffer_.resize(h265_fu_start_offset_);
                } else {
                    frame_buffer_.clear();
                }
            }
        }
        seq_initialized_ = true;
        last_seq_ = seq;

        const uint8_t cc = data[0] & 0x0F;
        const bool extension = (data[0] & 0x10) != 0;
        const bool padding = (data[0] & 0x20) != 0;
        size_t header_len = 12 + static_cast<size_t>(cc) * 4;
        if (header_len > len) {
            return;
        }
        if (extension) {
            if (header_len + 4 > len) {
                return;
            }
            const uint16_t ext_words = static_cast<uint16_t>((data[header_len + 2] << 8) | data[header_len + 3]);
            const size_t ext_len = 4 + static_cast<size_t>(ext_words) * 4;
            if (header_len + ext_len > len) {
                return;
            }
            header_len += ext_len;
        }
        size_t payload_len = len - header_len;
        if (padding) {
            if (payload_len == 0) {
                return;
            }
            const uint8_t pad_len = data[len - 1];
            if (pad_len == 0 || pad_len > payload_len) {
                return;
            }
            payload_len -= pad_len;
        }

        const uint8_t* payload = data + header_len;
        if (payload_len == 0) {
            return;
        }

        processVideoPayload(payload, payload_len, timestamp, marker, payload_type);
    }

    void processVideoPayload(const uint8_t* data, size_t len, uint32_t timestamp,
                             bool marker, uint8_t payload_type) {
        if (len == 0) {
            return;
        }

        if (!frame_in_progress_) {
            frame_ts_ = timestamp;
            frame_in_progress_ = true;
        } else if (timestamp != frame_ts_) {
            if (codec_ == CodecType::H265 && h265_fu_drop_mode_) {
                clearCurrentFrameState();
                h265_fu_drop_mode_ = false;
                h265_fu_in_progress_ = false;
            } else {
                emitFrame(frame_ts_);
            }
            frame_ts_ = timestamp;
            frame_in_progress_ = true;
        }

        const bool is_h264 = (payload_type == payload_type_ && codec_ == CodecType::H264);
        if (is_h264) {
            const uint8_t nal_type = data[0] & 0x1F;
            if (nal_type >= 1 && nal_type <= 23) {
                appendAnnexBNalu(data, len);
                if (nal_type == 5) {
                    frame_is_idr_ = true;
                }
            } else if (nal_type == 24) {
                size_t off = 1;
                while (off + 2 <= len) {
                    const uint16_t nalu_size = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
                    off += 2;
                    if (nalu_size == 0 || off + nalu_size > len) {
                        break;
                    }
                    const uint8_t inner_type = data[off] & 0x1F;
                    appendAnnexBNalu(data + off, nalu_size);
                    if (inner_type == 5) {
                        frame_is_idr_ = true;
                    }
                    off += nalu_size;
                }
            } else if (nal_type == 25) {
                if (len < 3) {
                    return;
                }
                size_t off = 3;
                while (off + 2 <= len) {
                    const uint16_t nalu_size = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
                    off += 2;
                    if (nalu_size == 0 || off + nalu_size > len) {
                        break;
                    }
                    const uint8_t inner_type = data[off] & 0x1F;
                    appendAnnexBNalu(data + off, nalu_size);
                    if (inner_type == 5) {
                        frame_is_idr_ = true;
                    }
                    off += nalu_size;
                }
            } else if (nal_type == 28 && len >= 2) {
                const uint8_t fu_header = data[1];
                const bool start = (fu_header & 0x80) != 0;
                const uint8_t reconstructed_nal = (data[0] & 0xE0) | (fu_header & 0x1F);
                if (start) {
                    static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
                    frame_buffer_.insert(frame_buffer_.end(), start_code, start_code + 4);
                    frame_buffer_.push_back(reconstructed_nal);
                    if ((reconstructed_nal & 0x1F) == 5) {
                        frame_is_idr_ = true;
                    }
                }
                if (len > 2) {
                    frame_buffer_.insert(frame_buffer_.end(), data + 2, data + len);
                }
            }
        } else {
            if (len < 2) {
                return;
            }
            const uint8_t nal_type = (data[0] >> 1) & 0x3F;
            if (nal_type != 49 && nal_type != 48 && nal_type != 50) {
                appendAnnexBNalu(data, len);
                if (isH265Irap(nal_type)) {
                    frame_is_idr_ = true;
                }
            } else if (nal_type == 48) {
                size_t off = 2;
                while (off + 2 <= len) {
                    const uint16_t nalu_size = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
                    off += 2;
                    if (nalu_size == 0 || off + nalu_size > len) {
                        break;
                    }
                    const uint8_t inner_type = (data[off] >> 1) & 0x3F;
                    appendAnnexBNalu(data + off, nalu_size);
                    if (isH265Irap(inner_type)) {
                        frame_is_idr_ = true;
                    }
                    off += nalu_size;
                }
            } else if (nal_type == 49 && len >= 3) {
                const uint8_t fu_indicator0 = data[0];
                const uint8_t fu_indicator1 = data[1];
                const uint8_t fu_header = data[2];
                const bool start = (fu_header & 0x80) != 0;
                const bool end = (fu_header & 0x40) != 0;
                const uint8_t orig_type = fu_header & 0x3F;
                const uint8_t orig0 = (fu_indicator0 & 0x81) | (orig_type << 1);
                const uint8_t orig1 = fu_indicator1;
                if (start) {
                    h265_fu_drop_mode_ = false;
                    h265_fu_in_progress_ = true;
                    h265_fu_start_offset_ = frame_buffer_.size();
                    static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
                    frame_buffer_.insert(frame_buffer_.end(), start_code, start_code + 4);
                    frame_buffer_.push_back(orig0);
                    frame_buffer_.push_back(orig1);
                    if (isH265Irap(orig_type)) {
                        frame_is_idr_ = true;
                    }
                } else if (h265_fu_drop_mode_ || !h265_fu_in_progress_) {
                    return;
                }
                if (len > 3 && !h265_fu_drop_mode_) {
                    frame_buffer_.insert(frame_buffer_.end(), data + 3, data + len);
                }
                if (end && h265_fu_in_progress_) {
                    h265_fu_in_progress_ = false;
                }
            }
        }

        if (marker) {
            if (codec_ == CodecType::H265 && h265_fu_drop_mode_) {
                clearCurrentFrameState();
                h265_fu_drop_mode_ = false;
                h265_fu_in_progress_ = false;
                return;
            }
            emitFrame(timestamp);
        }
    }

    Socket rtp_socket_;
    Socket rtcp_socket_;
    uint16_t rtp_port_ = 0;
    uint16_t rtcp_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread receive_thread_;
    FrameCallback callback_;

    CodecType codec_ = CodecType::H264;
    uint8_t payload_type_ = 96;
    uint32_t width_ = 1920;
    uint32_t height_ = 1080;
    uint32_t fps_ = 30;

    std::vector<uint8_t> frame_buffer_;
    uint32_t frame_ts_ = 0;
    bool frame_in_progress_ = false;
    bool frame_is_idr_ = false;
    bool seq_initialized_ = false;
    uint16_t last_seq_ = 0;
    bool h265_fu_in_progress_ = false;
    bool h265_fu_drop_mode_ = false;
    size_t h265_fu_start_offset_ = 0;
    uint32_t jitter_buffer_packets_ = 32;
    std::map<uint16_t, std::vector<uint8_t>> reorder_buffer_;
    bool reorder_initialized_ = false;
    uint16_t expected_seq_ = 0;
};

bool parseAnnouncedPathConfig(const std::string& path,
                              const std::string& sdp,
                              PathConfig* config,
                              uint8_t* payload_type) {
    if (config == nullptr || payload_type == nullptr) {
        return false;
    }

    SdpParser parser;
    if (!parser.parse(sdp) || !parser.hasVideo()) {
        return false;
    }

    const SdpMediaInfo video = parser.getVideoInfo();
    const std::string payload_name_upper = toLowerCopy(video.payload_name);
    const bool is_h264 = payload_name_upper.find("264") != std::string::npos;
    const bool is_h265 = payload_name_upper.find("265") != std::string::npos ||
                         payload_name_upper.find("hevc") != std::string::npos;
    if ((!is_h264 && !is_h265) || video.payload_type == 0 || video.clock_rate == 0) {
        return false;
    }

    config->path = path;
    config->codec = is_h265 ? CodecType::H265 : CodecType::H264;
    config->width = video.width != 0 ? video.width : 1920;
    config->height = video.height != 0 ? video.height : 1080;
    config->fps = video.fps != 0 ? video.fps : 30;
    config->sps = video.sps.empty() ? std::vector<uint8_t>() : base64Decode(video.sps);
    config->pps = video.pps.empty() ? std::vector<uint8_t>() : base64Decode(video.pps);
    config->vps = video.vps.empty() ? std::vector<uint8_t>() : base64Decode(video.vps);
    *payload_type = video.payload_type;
    return true;
}

struct ServerRegistry {
    std::mutex mutex;
    std::unordered_map<uint16_t, std::weak_ptr<RtspServer>> instances;
    std::unordered_map<uint16_t, std::string> first_host;
};

ServerRegistry& globalServerRegistry() {
    static ServerRegistry registry;
    return registry;
}

} // namespace

// 从完整RTSP URL中提取路径部分
// 支持格式: rtsp://host:port/path 或 /path
static std::string extractPathFromUrl(const std::string& url) {
    if (url.empty()) return "/";
    
    // 如果已经是纯路径（以/开头且没有scheme），直接返回
    if (url[0] == '/' && url.find("://") == std::string::npos) {
        return url;
    }
    
    // 去除 scheme (rtsp://)
    std::string temp = url;
    size_t scheme_pos = temp.find("://");
    if (scheme_pos != std::string::npos) {
        temp = temp.substr(scheme_pos + 3);
    }
    
    // 去除 host:port，提取路径
    size_t path_pos = temp.find('/');
    if (path_pos != std::string::npos) {
        // 去除查询参数
        size_t query_pos = temp.find('?', path_pos);
        if (query_pos != std::string::npos) {
            return temp.substr(path_pos, query_pos - path_pos);
        }
        return temp.substr(path_pos);
    }
    
    return "/";
}

// 生成随机session ID
static std::string generateSessionId() {
    static std::atomic<uint32_t> counter{0};
    uint32_t val = counter++;
    auto now = std::chrono::steady_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    
    std::stringstream ss;
    ss << std::hex << (ts & 0xFFFFFFFF) << val;
    return ss.str();
}

static std::string generateNonce() {
    return generateSessionId();
}

static std::unordered_map<std::string, std::string> parseAuthParams(const std::string& header_value) {
    std::unordered_map<std::string, std::string> kv;
    size_t pos = 0;
    while (pos < header_value.size()) {
        while (pos < header_value.size() && (header_value[pos] == ' ' || header_value[pos] == ',')) pos++;
        size_t eq = header_value.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = header_value.substr(pos, eq - pos);
        pos = eq + 1;
        std::string val;
        if (pos < header_value.size() && header_value[pos] == '"') {
            size_t endq = header_value.find('"', pos + 1);
            if (endq == std::string::npos) break;
            val = header_value.substr(pos + 1, endq - pos - 1);
            pos = endq + 1;
        } else {
            size_t comma = header_value.find(',', pos);
            if (comma == std::string::npos) {
                val = header_value.substr(pos);
                pos = header_value.size();
            } else {
                val = header_value.substr(pos, comma - pos);
                pos = comma + 1;
            }
        }
        kv[key] = val;
    }
    return kv;
}

struct ServerStatsAtomic {
    std::atomic<uint64_t> requests_total{0};
    std::atomic<uint64_t> auth_challenges{0};
    std::atomic<uint64_t> auth_failures{0};
    std::atomic<uint64_t> sessions_created{0};
    std::atomic<uint64_t> sessions_closed{0};
    std::atomic<uint64_t> frames_pushed{0};
    std::atomic<uint64_t> rtp_packets_sent{0};
    std::atomic<uint64_t> rtp_bytes_sent{0};
};

enum class SessionRole {
    Player,
    Publisher
};

// 客户端会话
struct ClientSession {
    std::string session_id;
    std::string path;
    std::string client_ip;
    SessionRole role = SessionRole::Player;
    uint16_t client_rtp_port = 0;
    uint16_t client_rtcp_port = 0;

    std::unique_ptr<RtpSender> rtp_sender;
    std::unique_ptr<RtpPacker> rtp_packer;
    std::unique_ptr<PublishRtpReceiver> rtp_receiver;
    uint8_t publisher_payload_type = 96;
    bool use_tcp_interleaved = false;
    uint8_t interleaved_rtp_channel = 0;
    std::shared_ptr<Socket> control_socket;
    std::shared_ptr<std::mutex> control_send_mutex;
    
    std::atomic<bool> playing{false};
    std::thread send_thread;
    
    // 帧队列
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::queue<VideoFrame> frame_queue;
    static constexpr size_t MAX_QUEUE_SIZE = 30;
    
    std::atomic<uint32_t> packet_count{0};
    std::atomic<uint32_t> octet_count{0};
    
    std::chrono::steady_clock::time_point last_activity;
    ServerStatsAtomic* stats = nullptr;
    
    ClientSession() {
        last_activity = std::chrono::steady_clock::now();
    }
    
    ~ClientSession() {
        stop();
    }
    
    void stop() {
        playing = false;
        queue_cv.notify_all();
        if (send_thread.joinable()) {
            send_thread.join();
        }
        if (rtp_receiver) {
            rtp_receiver->stop();
        }
        
        // 清理队列
        std::lock_guard<std::mutex> lock(queue_mutex);
        while (!frame_queue.empty()) {
            auto& frame = frame_queue.front();
            freeVideoFrame(frame);
            frame_queue.pop();
        }
    }
    
    bool pushFrame(const VideoFrame& frame) {
        if (role != SessionRole::Player) {
            return false;
        }

        std::lock_guard<std::mutex> lock(queue_mutex);
        if (frame_queue.size() >= MAX_QUEUE_SIZE) {
            // 队列满，丢弃最旧的帧
            auto& old = frame_queue.front();
            freeVideoFrame(old);
            frame_queue.pop();
        }
        
        // 复制帧
        VideoFrame copy = cloneFrameManaged(frame);
        
        frame_queue.push(copy);
        queue_cv.notify_one();
        return true;
    }
    
    void sendLoop() {
        while (playing) {
            VideoFrame frame;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [this] { return !frame_queue.empty() || !playing; });
                
                if (!playing) break;
                if (frame_queue.empty()) continue;
                
                frame = frame_queue.front();
                frame_queue.pop();
            }
            
            // 打包并发送
            if (rtp_packer && (use_tcp_interleaved || rtp_sender)) {
                auto packets = rtp_packer->packFrame(frame);
                
                for (const auto& packet : packets) {
                    if (use_tcp_interleaved) {
                        if (control_socket && control_socket->isValid() && control_send_mutex) {
                            std::vector<uint8_t> interleaved(4 + packet.size);
                            interleaved[0] = '$';
                            interleaved[1] = interleaved_rtp_channel;
                            interleaved[2] = static_cast<uint8_t>((packet.size >> 8) & 0xFF);
                            interleaved[3] = static_cast<uint8_t>(packet.size & 0xFF);
                            memcpy(interleaved.data() + 4, packet.data, packet.size);
                            std::lock_guard<std::mutex> sock_lock(*control_send_mutex);
                            control_socket->send(interleaved.data(), interleaved.size());
                        }
                    } else if (rtp_sender) {
                        rtp_sender->sendRtpPacket(packet);
                    }
                    packet_count++;
                    octet_count += packet.size;
                    if (stats) {
                        stats->rtp_packets_sent++;
                        stats->rtp_bytes_sent += packet.size;
                    }
                    delete[] packet.data;
                }
                last_activity = std::chrono::steady_clock::now();
                
                // RTCP SR is only valid for UDP sender sessions.
                if (!use_tcp_interleaved && rtp_sender && (packet_count % 100 == 0)) {
                    auto now = std::chrono::system_clock::now();
                    auto epoch = now.time_since_epoch();
                    uint64_t ntp_ts = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
                    ntp_ts = (ntp_ts + 2208988800u) << 32;  // NTP epoch offset
                    
                    uint32_t rtp_ts = convertToRtpTimestamp(frame.pts, 90000);
                    rtp_sender->sendSenderReport(rtp_ts, ntp_ts, packet_count.load(), octet_count.load());
                }
            }
            
            freeVideoFrame(frame);
        }
    }
};

// 媒体路径
struct MediaPath {
    std::string path;
    PathConfig config;
    
    std::mutex sessions_mutex;
    std::map<std::string, std::shared_ptr<ClientSession>> sessions;
    
    // 最新帧（用于新连接的客户端）
    std::mutex latest_frame_mutex;
    VideoFrame latest_frame;
    bool has_latest_frame = false;
    
    void broadcastFrame(const VideoFrame& frame) {
        // 更新最新帧
        {
            std::lock_guard<std::mutex> lock(latest_frame_mutex);
            freeVideoFrame(latest_frame);
            latest_frame = cloneFrameManaged(frame);
            has_latest_frame = true;
        }
        
        // 广播到所有客户端
        std::lock_guard<std::mutex> lock(sessions_mutex);
        for (auto& session_pair : sessions) {
            auto& session = session_pair.second;
            if (session->playing) {
                session->pushFrame(frame);
            }
        }
    }
    
    void addSession(const std::string& session_id, std::shared_ptr<ClientSession> session) {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        sessions[session_id] = session;
        
        // 发送最新帧给新客户端（关键帧优先）
        std::lock_guard<std::mutex> lf_lock(latest_frame_mutex);
        if (has_latest_frame && latest_frame.type == FrameType::IDR) {
            session->pushFrame(latest_frame);
        }
    }
    
    void removeSession(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        auto it = sessions.find(session_id);
        if (it != sessions.end()) {
            it->second->stop();
            sessions.erase(it);
        }
    }
    
    ~MediaPath() {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        for (auto& session_pair : sessions) {
            auto& session = session_pair.second;
            session->stop();
        }
        sessions.clear();
        
        freeVideoFrame(latest_frame);
    }
};

// RTSP连接处理
class RtspConnection {
public:
    RtspConnection(std::shared_ptr<Socket> socket,
                   std::map<std::string, std::shared_ptr<MediaPath>>& paths,
                   std::mutex& paths_mutex,
                   RtspServerConfig& config,
                   RtspServer::ClientConnectCallback& connect_cb,
                   RtspServer::ClientDisconnectCallback& disconnect_cb,
                   ServerStatsAtomic& stats)
        : socket_(std::move(socket)),
          paths_(paths),
          paths_mutex_(paths_mutex),
          config_(config),
          connect_cb_(connect_cb),
          disconnect_cb_(disconnect_cb),
          send_mutex_(std::make_shared<std::mutex>()),
          stats_(stats),
          digest_nonce_(config.auth_nonce.empty() ? "nonce-" + generateNonce() : config.auth_nonce),
          digest_nonce_created_(std::chrono::steady_clock::now()) {}
    
    void handle() {
        std::string buffer;
        buffer.reserve(4096);
        
        uint8_t temp[4096];
        while (socket_->isValid()) {
            ssize_t n = socket_->recv(temp, sizeof(temp), 1000);
            if (n > 0) {
                buffer.append((char*)temp, n);

                // TCP interleaved mode: the client may send interleaved RTP/RTCP packets (start with '$')
                // on the control socket. If we try to parse these bytes as RTSP text, we may emit bogus
                // RTSP responses and make players (ffmpeg/ffplay) tear down the session.
                //
                // We only need to consume/ignore these packets to keep the RTSP request parser in sync.
                while (!buffer.empty()) {
                    if (buffer[0] == '$') {
                        if (buffer.size() < 4) {
                            break;  // wait more data
                        }
                        const std::uint16_t len =
                            (static_cast<std::uint8_t>(buffer[2]) << 8U) |
                            static_cast<std::uint8_t>(buffer[3]);
                        const std::size_t total = 4U + static_cast<std::size_t>(len);
                        if (buffer.size() < total) {
                            break;  // wait more data
                        }
                        buffer.erase(0, total);
                        if (session_) {
                            session_->last_activity = std::chrono::steady_clock::now();
                        }
                        continue;
                    }

                    // 检查是否收到完整请求
                    const size_t pos = buffer.find("\r\n\r\n");
                    if (pos == std::string::npos) {
                        break;
                    }
                    size_t content_length = 0;
                    size_t header_end = pos + 4;

                    // 解析Content-Length
                    std::string header = buffer.substr(0, header_end);
                    std::string header_lower = header;
                    std::transform(header_lower.begin(), header_lower.end(), header_lower.begin(), ::tolower);
                    size_t cl_pos = header_lower.find("content-length:");
                    if (cl_pos != std::string::npos) {
                        size_t cl_end = header_lower.find("\r\n", cl_pos);
                        if (cl_end != std::string::npos) {
                            std::string cl_str = header.substr(cl_pos + 15, cl_end - cl_pos - 15);
                            try {
                                content_length = static_cast<size_t>(std::stoul(cl_str));
                            } catch (...) {
                                // 非法Content-Length，丢弃当前缓冲避免崩溃
                                buffer.clear();
                                break;
                            }
                        }
                    }

                    // 检查是否有足够的主体数据
                    if (buffer.size() < header_end + content_length) {
                        break;
                    }

                    // 处理请求
                    std::string request_data = buffer.substr(0, header_end + content_length);
                    buffer.erase(0, header_end + content_length);

                    processRequest(request_data);
                }
            } else if (n == 0) {
                // 连接关闭
                break;
            }
            // n < 0 是超时，继续循环
        }
        
        // 清理会话
        if (session_) {
            auto path_it = paths_.find(session_->path);
            if (path_it != paths_.end()) {
                path_it->second->removeSession(session_->session_id);
            }
            if (disconnect_cb_ && session_->role == SessionRole::Player) {
                disconnect_cb_(session_->path, session_->client_ip);
            }
        }
    }

private:
    void processRequest(const std::string& data) {
        RtspRequest request;
        if (!request.parse(data)) {
            return;
        }
        stats_.requests_total++;
        
        int cseq = request.getCSeq();
        if (!checkAuthorization(request, cseq)) {
            return;
        }
        if (session_) {
            session_->last_activity = std::chrono::steady_clock::now();
        }
        
        RTSP_LOG_INFO("RTSP " + RtspRequest::methodToString(request.getMethod()) + 
                      " " + request.getUri());
        
        switch (request.getMethod()) {
            case RtspMethod::Options:
                sendResponse(RtspResponse::createOptions(cseq));
                break;
                
            case RtspMethod::Describe:
                handleDescribe(request, cseq);
                break;

            case RtspMethod::Announce:
                handleAnnounce(request, cseq);
                break;
                
            case RtspMethod::Setup:
                handleSetup(request, cseq);
                break;
                
            case RtspMethod::Play:
                handlePlay(request, cseq);
                break;

            case RtspMethod::Record:
                handleRecord(request, cseq);
                break;

            case RtspMethod::Pause:
                handlePause(request, cseq);
                break;

            case RtspMethod::GetParameter:
                handleGetParameter(request, cseq);
                break;

            case RtspMethod::SetParameter:
                handleSetParameter(request, cseq);
                break;
                
            case RtspMethod::Teardown:
                handleTeardown(request, cseq);
                break;
                
            default:
                sendResponse(RtspResponse::createError(cseq, 501, "Not Implemented"));
                break;
        }
    }
    
    void handleDescribe(const RtspRequest& request, int cseq) {
        std::string path = extractPathFromUrl(request.getPath());
        
        std::lock_guard<std::mutex> lock(paths_mutex_);
        auto it = paths_.find(path);
        if (it == paths_.end()) {
            sendResponse(RtspResponse::createError(cseq, 404, "Not Found"));
            return;
        }
        
        auto& media_path = it->second;
        auto& config = media_path->config;
        
        // 构建SDP
        SdpBuilder sdp;
        // 使用 0.0.0.0 表示接受任何地址的连接
        sdp.setConnection("IN", "IP4", "0.0.0.0");
        
        // 计算payload type
        uint8_t payload_type = (config.codec == CodecType::H264) ? 96 : 97;
        uint32_t clock_rate = 90000;
        
        std::string sps_b64 = base64Encode(config.sps.data(), config.sps.size());
        std::string pps_b64 = base64Encode(config.pps.data(), config.pps.size());
        std::string vps_b64 = base64Encode(config.vps.data(), config.vps.size());
        
        std::string control = "stream";
        
        if (config.codec == CodecType::H264) {
            sdp.addH264Media(control, 0, payload_type, clock_rate,
                            sps_b64, pps_b64, config.width, config.height);
        } else {
            sdp.addH265Media(control, 0, payload_type, clock_rate,
                            vps_b64, sps_b64, pps_b64, config.width, config.height);
        }
        
        sendResponse(RtspResponse::createDescribe(cseq, sdp.build()));
    }

    void handleAnnounce(const RtspRequest& request, int cseq) {
        if (session_) {
            sendResponse(RtspResponse::createError(cseq, 459, "Aggregate Operation Not Allowed"));
            return;
        }

        std::string path = extractPathFromUrl(request.getPath());

        PathConfig announced_config{};
        uint8_t payload_type = 96;
        if (!parseAnnouncedPathConfig(path, request.getBody(), &announced_config, &payload_type)) {
            sendResponse(RtspResponse::createError(cseq, 400, "Bad Request"));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(paths_mutex_);
            auto it = paths_.find(path);
            if (it == paths_.end()) {
                auto media_path = std::make_shared<MediaPath>();
                media_path->path = path;
                media_path->config = announced_config;
                paths_[path] = media_path;
            } else {
                it->second->config.codec = announced_config.codec;
                it->second->config.width = announced_config.width;
                it->second->config.height = announced_config.height;
                it->second->config.fps = announced_config.fps;
                if (!announced_config.sps.empty()) {
                    it->second->config.sps = announced_config.sps;
                }
                if (!announced_config.pps.empty()) {
                    it->second->config.pps = announced_config.pps;
                }
                if (!announced_config.vps.empty()) {
                    it->second->config.vps = announced_config.vps;
                }
            }
        }

        session_ = std::make_shared<ClientSession>();
        session_->session_id = generateSessionId();
        session_->path = path;
        session_->client_ip = socket_->getPeerIp();
        session_->role = SessionRole::Publisher;
        session_->control_socket = socket_;
        session_->control_send_mutex = send_mutex_;
        session_->publisher_payload_type = payload_type;

        RtspResponse response = RtspResponse::createOk(cseq);
        response.setSession(session_->session_id);
        sendResponse(response);
    }

    void handlePlayerSetup(const RtspRequest& request, int cseq) {
        if (session_) {
            sendResponse(RtspResponse::createError(cseq, 459, "Aggregate Operation Not Allowed"));
            return;
        }

        std::string path = extractPathFromUrl(request.getPath());
        size_t slash_pos = path.find_last_of('/');
        if (slash_pos != std::string::npos) {
            path = path.substr(0, slash_pos);
        }

        std::lock_guard<std::mutex> lock(paths_mutex_);
        auto it = paths_.find(path);
        if (it == paths_.end()) {
            sendResponse(RtspResponse::createError(cseq, 404, "Not Found"));
            return;
        }

        auto& media_path = it->second;

        std::string transport = request.getTransport();
        const bool use_tcp = (transport.find("RTP/AVP/TCP") != std::string::npos ||
                              transport.find("TCP") != std::string::npos);

        const int client_rtp_port = request.getRtpPort();
        const int client_rtcp_port = request.getRtcpPort();

        if (client_rtp_port == 0 && !use_tcp) {
            sendResponse(RtspResponse::createError(cseq, 400, "Bad Request"));
            return;
        }

        session_ = std::make_shared<ClientSession>();
        session_->session_id = generateSessionId();
        session_->path = path;
        session_->client_ip = socket_->getPeerIp();
        session_->role = SessionRole::Player;
        session_->client_rtp_port = static_cast<uint16_t>(client_rtp_port);
        session_->client_rtcp_port = static_cast<uint16_t>(client_rtcp_port != 0 ? client_rtcp_port : client_rtp_port + 1);
        session_->use_tcp_interleaved = use_tcp;
        session_->control_socket = socket_;
        session_->control_send_mutex = send_mutex_;
        session_->stats = &stats_;
        
        // 创建RTP发送器
        if (!use_tcp) {
            session_->rtp_sender = std::make_unique<RtpSender>();
            bool sender_ready = false;
            for (int attempt = 0; attempt < 32; ++attempt) {
                uint16_t local_rtp_port = RtspServerConfig::getNextRtpPort(
                    config_.rtp_port_current, config_.rtp_port_start, config_.rtp_port_end);
                if (session_->rtp_sender->init("0.0.0.0", local_rtp_port)) {
                    sender_ready = true;
                    break;
                }
            }
            if (!sender_ready) {
                sendResponse(RtspResponse::createError(cseq, 500, "Internal Server Error"));
                session_.reset();
                return;
            }
            
            session_->rtp_sender->setPeer(session_->client_ip, 
                                           session_->client_rtp_port, 
                                           session_->client_rtcp_port);
        }
        
        // 创建RTP打包器
        if (media_path->config.codec == CodecType::H264) {
            session_->rtp_packer = std::make_unique<H264RtpPacker>();
        } else {
            session_->rtp_packer = std::make_unique<H265RtpPacker>();
        }
        session_->rtp_packer->setPayloadType((media_path->config.codec == CodecType::H264) ? 96 : 97);
        session_->rtp_packer->setSsrc(0x12345678 + std::hash<std::string>{}(session_->session_id));
        
        // 添加到媒体路径
        media_path->addSession(session_->session_id, session_);
        stats_.sessions_created++;
        if (connect_cb_) {
            connect_cb_(session_->path, session_->client_ip);
        }
        
        // 重新计算 use_tcp（因为前面代码块中的变量作用域问题）
        bool is_tcp = use_tcp;
        if (is_tcp) {
            std::regex ch_re("interleaved=(\\d+)-(\\d+)", std::regex::icase);
            std::smatch m;
            if (std::regex_search(transport, m, ch_re)) {
                session_->interleaved_rtp_channel = static_cast<uint8_t>(std::stoi(m[1].str()));
            }
        }
        
        // 构建transport响应
        std::stringstream transport_ss;
        if (is_tcp) {
            // TCP 模式 (RTP over RTSP)
            transport_ss << "RTP/AVP/TCP;unicast;interleaved="
                         << static_cast<int>(session_->interleaved_rtp_channel) << "-"
                         << static_cast<int>(session_->interleaved_rtp_channel + 1);
        } else {
            // UDP 模式
            transport_ss << "RTP/AVP;unicast;client_port=" << client_rtp_port 
                         << "-" << session_->client_rtcp_port
                         << ";server_port=" << session_->rtp_sender->getLocalPort()
                         << "-" << session_->rtp_sender->getLocalRtcpPort();
        }
        
        sendResponse(RtspResponse::createSetup(cseq, session_->session_id, transport_ss.str()));
    }

    void handlePublisherSetup(const RtspRequest& request, int cseq) {
        if (!session_ || session_->role != SessionRole::Publisher) {
            sendResponse(RtspResponse::createError(cseq, 455, "Method Not Valid In This State"));
            return;
        }

        const std::string transport = request.getTransport();
        if (toLowerCopy(transport).find("tcp") != std::string::npos) {
            sendResponse(RtspResponse::createError(cseq, 461, "Unsupported Transport"));
            return;
        }

        const int client_rtp_port = request.getRtpPort();
        const int client_rtcp_port = request.getRtcpPort();
        if (client_rtp_port == 0) {
            sendResponse(RtspResponse::createError(cseq, 400, "Bad Request"));
            return;
        }

        std::shared_ptr<MediaPath> media_path;
        {
            std::lock_guard<std::mutex> lock(paths_mutex_);
            auto it = paths_.find(session_->path);
            if (it == paths_.end()) {
                sendResponse(RtspResponse::createError(cseq, 404, "Not Found"));
                return;
            }
            media_path = it->second;
        }

        session_->client_rtp_port = static_cast<uint16_t>(client_rtp_port);
        session_->client_rtcp_port = static_cast<uint16_t>(client_rtcp_port != 0 ? client_rtcp_port : client_rtp_port + 1);

        if (!session_->rtp_receiver) {
            auto receiver = std::make_unique<PublishRtpReceiver>();
            bool receiver_ready = false;
            for (int attempt = 0; attempt < 32; ++attempt) {
                const uint16_t local_rtp_port = RtspServerConfig::getNextRtpPort(
                    config_.rtp_port_current, config_.rtp_port_start, config_.rtp_port_end);
                if (receiver->init(local_rtp_port, static_cast<uint16_t>(local_rtp_port + 1))) {
                    receiver_ready = true;
                    break;
                }
            }
            if (!receiver_ready) {
                sendResponse(RtspResponse::createError(cseq, 500, "Internal Server Error"));
                return;
            }

            receiver->setVideoInfo(media_path->config.codec,
                                   media_path->config.width,
                                   media_path->config.height,
                                   media_path->config.fps,
                                   session_->publisher_payload_type);
            std::weak_ptr<MediaPath> weak_path = media_path;
            receiver->setCallback([weak_path, this](const VideoFrame& frame) {
                if (auto path = weak_path.lock()) {
                    if (frame.codec == CodecType::H264 &&
                        (frame.type == FrameType::IDR || path->config.sps.empty() || path->config.pps.empty())) {
                        (void)autoExtractH264ParameterSets(path->config, frame.data, frame.size);
                    } else if (frame.codec == CodecType::H265 &&
                               (frame.type == FrameType::IDR || path->config.vps.empty() ||
                                path->config.sps.empty() || path->config.pps.empty())) {
                        (void)autoExtractH265ParameterSets(path->config, frame.data, frame.size);
                    }
                    path->broadcastFrame(frame);
                    stats_.frames_pushed++;
                }
            });
            session_->rtp_receiver = std::move(receiver);

            media_path->addSession(session_->session_id, session_);
            stats_.sessions_created++;
        }

        std::stringstream transport_ss;
        transport_ss << "RTP/AVP;unicast;client_port=" << session_->client_rtp_port
                     << "-" << session_->client_rtcp_port
                     << ";server_port=" << session_->rtp_receiver->getRtpPort()
                     << "-" << session_->rtp_receiver->getRtcpPort();

        sendResponse(RtspResponse::createSetup(cseq, session_->session_id, transport_ss.str()));
    }

    void handleSetup(const RtspRequest& request, int cseq) {
        if (isRecordTransport(request.getTransport()) ||
            (session_ && session_->role == SessionRole::Publisher)) {
            handlePublisherSetup(request, cseq);
            return;
        }
        handlePlayerSetup(request, cseq);
    }
    
    void handlePlay(const RtspRequest& request, int cseq) {
        if (!session_) {
            sendResponse(RtspResponse::createError(cseq, 455, "Method Not Valid In This State"));
            return;
        }
        
        std::string session_id = request.getSession();
        if (session_id != session_->session_id) {
            sendResponse(RtspResponse::createError(cseq, 454, "Session Not Found"));
            return;
        }

        // 幂等处理：已在播放直接返回成功，避免重复创建线程导致崩溃
        if (!session_->playing) {
            session_->playing = true;
            if (!session_->send_thread.joinable()) {
                session_->send_thread = std::thread([this]() {
                    session_->sendLoop();
                });
            }
        }
        
        sendResponse(RtspResponse::createPlay(cseq, session_->session_id));
    }

    void handleRecord(const RtspRequest& request, int cseq) {
        if (!session_ || session_->role != SessionRole::Publisher || !session_->rtp_receiver) {
            sendResponse(RtspResponse::createError(cseq, 455, "Method Not Valid In This State"));
            return;
        }

        const std::string session_id = request.getSession();
        if (!session_id.empty() && session_id != session_->session_id) {
            sendResponse(RtspResponse::createError(cseq, 454, "Session Not Found"));
            return;
        }

        session_->rtp_receiver->start();
        session_->last_activity = std::chrono::steady_clock::now();

        RtspResponse response = RtspResponse::createOk(cseq);
        response.setSession(session_->session_id);
        sendResponse(response);
    }

    void handlePause(const RtspRequest& request, int cseq) {
        (void)request;
        if (!session_) {
            sendResponse(RtspResponse::createError(cseq, 455, "Method Not Valid In This State"));
            return;
        }
        session_->stop();
        sendResponse(RtspResponse::createOk(cseq));
    }

    void handleGetParameter(const RtspRequest& request, int cseq) {
        // Keep-alive support: session存在则返回200，否则返回454
        std::string sid = request.getSession();
        if (!session_ || (!sid.empty() && sid != session_->session_id)) {
            sendResponse(RtspResponse::createError(cseq, 454, "Session Not Found"));
            return;
        }
        sendResponse(RtspResponse::createOk(cseq));
    }

    void handleSetParameter(const RtspRequest& request, int cseq) {
        // Keep-alive / parameter set: 当前实现接受请求但不持久化参数
        std::string sid = request.getSession();
        if (!session_ || (!sid.empty() && sid != session_->session_id)) {
            sendResponse(RtspResponse::createError(cseq, 454, "Session Not Found"));
            return;
        }
        sendResponse(RtspResponse::createOk(cseq));
    }
    
    void handleTeardown(const RtspRequest& request, int cseq) {
        (void)request;
        if (session_) {
            std::shared_ptr<MediaPath> media_path;
            {
                std::lock_guard<std::mutex> lock(paths_mutex_);
                auto path_it = paths_.find(session_->path);
                if (path_it != paths_.end()) {
                    media_path = path_it->second;
                }
            }
            if (media_path) {
                media_path->removeSession(session_->session_id);
            }
            stats_.sessions_closed++;
            if (disconnect_cb_ && session_->role == SessionRole::Player) {
                disconnect_cb_(session_->path, session_->client_ip);
            }
            session_.reset();
        }
        
        sendResponse(RtspResponse::createTeardown(cseq));
    }
    
    void sendResponse(const RtspResponse& response) {
        std::string data = response.build();
        std::lock_guard<std::mutex> lock(*send_mutex_);
        socket_->send((const uint8_t*)data.c_str(), data.size());
    }

    bool checkAuthorization(const RtspRequest& request, int cseq) {
        if (!config_.auth_enabled) return true;
        if (request.getMethod() == RtspMethod::Options) return true;

        std::string auth = request.getHeader("Authorization");
        auto reject = [&](bool digest, bool stale) {
            RtspResponse resp(cseq);
            resp.setStatus(401, "Unauthorized");
            stats_.auth_challenges++;
            stats_.auth_failures++;
            if (digest) {
                resp.setHeader("WWW-Authenticate",
                               "Digest realm=\"" + config_.auth_realm +
                               "\", nonce=\"" + digest_nonce_ +
                               "\", algorithm=MD5, qop=\"auth\"" +
                               std::string(stale ? ", stale=true" : ""));
            } else {
                resp.setHeader("WWW-Authenticate", "Basic realm=\"" + config_.auth_realm + "\"");
            }
            sendResponse(resp);
            return false;
        };

        if (config_.auth_use_digest) {
            auto now = std::chrono::steady_clock::now();
            if (now - digest_nonce_created_ > std::chrono::milliseconds(config_.auth_nonce_ttl_ms)) {
                digest_nonce_ = "nonce-" + generateNonce();
                digest_nonce_created_ = now;
                digest_nc_seen_.clear();
                return reject(true, true);
            }
            if (auth.rfind("Digest ", 0) != 0) {
                return reject(true, false);
            }
            auto params = parseAuthParams(auth.substr(7));
            const std::string username = params["username"];
            const std::string realm = params["realm"];
            const std::string header_nonce = params["nonce"];
            const std::string uri = params["uri"];
            const std::string response = params["response"];
            const std::string qop = params["qop"];
            const std::string nc = params["nc"];
            const std::string cnonce = params["cnonce"];

            if (username.empty() || realm.empty() || header_nonce.empty() || uri.empty() || response.empty()) {
                return reject(true, false);
            }
            if (username != config_.auth_username || realm != config_.auth_realm || header_nonce != digest_nonce_) {
                return reject(true, false);
            }

            std::string ha1 = md5Hex(config_.auth_username + ":" + config_.auth_realm + ":" + config_.auth_password);
            std::string ha2 = md5Hex(RtspRequest::methodToString(request.getMethod()) + ":" + uri);
            std::string expected;
            if (!qop.empty()) {
                if (nc.empty() || cnonce.empty()) {
                    return reject(true, false);
                }
                uint64_t nc_value = 0;
                try {
                    nc_value = std::stoull(nc, nullptr, 16);
                } catch (...) {
                    return reject(true, false);
                }
                std::string nc_key = username + "|" + cnonce + "|" + header_nonce;
                auto it = digest_nc_seen_.find(nc_key);
                if (it != digest_nc_seen_.end() && nc_value <= it->second) {
                    return reject(true, false);
                }
                digest_nc_seen_[nc_key] = nc_value;
                expected = md5Hex(ha1 + ":" + header_nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
            } else {
                expected = md5Hex(ha1 + ":" + header_nonce + ":" + ha2);
            }
            if (expected != response) {
                return reject(true, false);
            }
            return true;
        }

        if (auth.rfind("Basic ", 0) != 0) {
            return reject(false, false);
        }
        std::string encoded = auth.substr(6);
        auto decoded = base64Decode(encoded);
        std::string userpass(decoded.begin(), decoded.end());
        std::string expected = config_.auth_username + ":" + config_.auth_password;
        if (userpass != expected) {
            return reject(false, false);
        }
        return true;
    }

private:
    std::shared_ptr<Socket> socket_;
    std::map<std::string, std::shared_ptr<MediaPath>>& paths_;
    std::mutex& paths_mutex_;
    RtspServerConfig& config_;
    RtspServer::ClientConnectCallback& connect_cb_;
    RtspServer::ClientDisconnectCallback& disconnect_cb_;
    std::shared_ptr<std::mutex> send_mutex_;
    ServerStatsAtomic& stats_;
    std::shared_ptr<ClientSession> session_;
    std::string digest_nonce_;
    std::chrono::steady_clock::time_point digest_nonce_created_;
    std::unordered_map<std::string, uint64_t> digest_nc_seen_;
};

// RtspServer实现
class RtspServer::Impl {
public:
    RtspServerConfig config_;
    std::atomic<bool> running_{false};
    std::unique_ptr<TcpServer> tcp_server_;
    std::thread server_thread_;
    
    std::mutex paths_mutex_;
    std::map<std::string, std::shared_ptr<MediaPath>> paths_;
    
    ClientConnectCallback connect_callback_;
    ClientDisconnectCallback disconnect_callback_;
    ServerStatsAtomic stats_;

    std::mutex connections_mutex_;
    struct ConnectionHandle {
        std::shared_ptr<Socket> socket;
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<ConnectionHandle> connections_;
    
    std::thread cleanup_thread_;
    std::mutex cleanup_mutex_;
    std::condition_variable cleanup_cv_;
    
    void cleanupFinishedConnections() {
        std::vector<ConnectionHandle> finished;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (auto it = connections_.begin(); it != connections_.end();) {
                if (it->done && it->done->load()) {
                    finished.push_back({std::move(it->socket), std::move(it->thread), std::move(it->done)});
                    it = connections_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& handle : finished) {
            if (handle.socket) {
                handle.socket->close();
                handle.socket.reset();
            }
            if (handle.thread.joinable()) {
                handle.thread.join();
            }
        }
    }

    void cleanupLoop() {
        while (true) {
            std::unique_lock<std::mutex> wait_lock(cleanup_mutex_);
            cleanup_cv_.wait_for(wait_lock, std::chrono::milliseconds(100), [this]() {
                return !running_.load();
            });
            if (!running_) {
                break;
            }
            wait_lock.unlock();

            cleanupFinishedConnections();
            
            // 清理超时会话
            std::vector<std::pair<std::string, std::string>> disconnects;
            {
                std::lock_guard<std::mutex> lock(paths_mutex_);
                auto now = std::chrono::steady_clock::now();
                for (auto& path_pair : paths_) {
                    auto& path = path_pair.second;
                    std::lock_guard<std::mutex> session_lock(path->sessions_mutex);
                    for (auto it = path->sessions.begin(); it != path->sessions.end();) {
                        auto last_activity = it->second->last_activity;
                        if (now - last_activity > std::chrono::milliseconds(config_.session_timeout_ms)) {
                            RTSP_LOG_INFO("Session timeout: " + it->first);
                            it->second->stop();
                            stats_.sessions_closed++;
                            if (it->second->role == SessionRole::Player) {
                                disconnects.emplace_back(path->path, it->second->client_ip);
                            }
                            it = path->sessions.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }
            for (const auto& d : disconnects) {
                if (disconnect_callback_) {
                    disconnect_callback_(d.first, d.second);
                }
            }
        }

        cleanupFinishedConnections();
    }
};

uint16_t RtspServerConfig::getNextRtpPort(uint32_t& current, uint32_t start, uint32_t end) {
    uint16_t port = (uint16_t)current;
    current += 2;
    if (current >= end) {
        current = start;
    }
    return port;
}

RtspServer::RtspServer() : impl_(std::make_unique<Impl>()) {}
RtspServer::~RtspServer() {
    stop();
}

bool RtspServer::init(const RtspServerConfig& config) {
    impl_->config_ = config;
    return true;
}

bool RtspServer::init(const std::string& host, uint16_t port) {
    impl_->config_.host = host;
    impl_->config_.port = port;
    return true;
}

bool RtspServer::start() {
    if (impl_->running_) return false;
    
    impl_->tcp_server_ = std::make_unique<TcpServer>();
    
    impl_->tcp_server_->setNewConnectionCallback([this](std::unique_ptr<Socket> socket) {
        auto shared_socket = std::shared_ptr<Socket>(std::move(socket));
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread conn_thread([this, s = shared_socket, done]() mutable {
            RtspConnection conn(s, impl_->paths_, impl_->paths_mutex_, impl_->config_,
                                impl_->connect_callback_, impl_->disconnect_callback_,
                                impl_->stats_);
            conn.handle();
            if (s) {
                s->close();
            }
            done->store(true);
        });
        {
            std::lock_guard<std::mutex> lock(impl_->connections_mutex_);
            impl_->connections_.push_back({shared_socket, std::move(conn_thread), done});
        }
    });
    
    if (!impl_->tcp_server_->start(impl_->config_.host, impl_->config_.port)) {
        RTSP_LOG_ERROR("Failed to start RTSP server on " + impl_->config_.host + 
                       ":" + std::to_string(impl_->config_.port));
        return false;
    }
    
    impl_->running_ = true;
    impl_->cleanup_thread_ = std::thread([this]() {
        impl_->cleanupLoop();
    });
    
    RTSP_LOG_INFO("RTSP server started on " + impl_->config_.host + 
                  ":" + std::to_string(impl_->config_.port));
    
    return true;
}

void RtspServer::stop() {
    stopWithTimeout(5000);
}

bool RtspServer::stopWithTimeout(uint32_t timeout_ms) {
    const auto begin = std::chrono::steady_clock::now();
    RTSP_LOG_INFO("RtspServer stop start, timeout_ms=" + std::to_string(timeout_ms));
    impl_->running_ = false;
    impl_->cleanup_cv_.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    
    if (impl_->tcp_server_) {
        impl_->tcp_server_->stop();
    }
    
    std::vector<Impl::ConnectionHandle> connections;
    {
        std::lock_guard<std::mutex> lock(impl_->connections_mutex_);
        for (auto& c : impl_->connections_) {
            if (c.socket) {
                c.socket->close();
            }
        }
        connections = std::move(impl_->connections_);
        impl_->connections_.clear();
    }
    bool all_joined = true;
    for (auto& c : connections) {
        uint32_t remain = 0;
        auto now = std::chrono::steady_clock::now();
        if (now < deadline) {
            remain = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        }
        if (!joinThreadWithTimeout(c.thread, remain)) {
            all_joined = false;
            RTSP_LOG_ERROR("RtspServer stop timeout: connection thread still alive (blocking: RTSP connection loop)");
        }
    }

    {
        uint32_t remain = 0;
        auto now = std::chrono::steady_clock::now();
        if (now < deadline) {
            remain = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        }
        if (!joinThreadWithTimeout(impl_->cleanup_thread_, remain)) {
            all_joined = false;
            RTSP_LOG_ERROR("RtspServer stop timeout: cleanup_thread still alive (blocking: cleanup loop sleep/wait)");
        }
    }
    
    // 清理所有路径
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    impl_->paths_.clear();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    RTSP_LOG_INFO("RtspServer stop done, elapsed_ms=" + std::to_string(elapsed));
    return all_joined;
}

bool RtspServer::isRunning() const {
    return impl_->running_;
}

bool RtspServer::addPath(const PathConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    
    if (impl_->paths_.find(config.path) != impl_->paths_.end()) {
        return false;
    }
    
    auto path = std::make_shared<MediaPath>();
    path->path = config.path;
    path->config = config;
    
    impl_->paths_[config.path] = path;
    
    RTSP_LOG_INFO("Added path: " + config.path);
    return true;
}

bool RtspServer::addPath(const std::string& path, CodecType codec) {
    PathConfig config;
    config.path = path;
    config.codec = codec;
    return addPath(config);
}

bool RtspServer::removePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    return impl_->paths_.erase(path) > 0;
}

bool RtspServer::pushFrame(const std::string& path, const VideoFrame& frame) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    
    auto it = impl_->paths_.find(path);
    if (it == impl_->paths_.end()) {
        return false;
    }
    
    it->second->broadcastFrame(frame);
    impl_->stats_.frames_pushed++;
    return true;
}

bool RtspServer::pushH264Data(const std::string& path, const uint8_t* data, size_t size,
                               uint64_t pts, bool is_key) {
    VideoFrame frame = {};
    frame.codec = CodecType::H264;
    frame.type = is_key ? FrameType::IDR : FrameType::P;
    frame.data = const_cast<uint8_t*>(data);
    frame.size = size;
    frame.pts = pts;
    frame.dts = pts;
    
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    auto it = impl_->paths_.find(path);
    if (it == impl_->paths_.end()) {
        return false;
    }

    bool updated = false;
    if (is_key || it->second->config.sps.empty() || it->second->config.pps.empty()) {
        updated = autoExtractH264ParameterSets(it->second->config, data, size);
    }
    if (updated) {
        RTSP_LOG_INFO("Auto-updated H264 parameter sets for path: " + path);
    }

    it->second->broadcastFrame(frame);
    impl_->stats_.frames_pushed++;
    return true;
}

bool RtspServer::pushH265Data(const std::string& path, const uint8_t* data, size_t size,
                               uint64_t pts, bool is_key) {
    VideoFrame frame = {};
    frame.codec = CodecType::H265;
    frame.type = is_key ? FrameType::IDR : FrameType::P;
    frame.data = const_cast<uint8_t*>(data);
    frame.size = size;
    frame.pts = pts;
    frame.dts = pts;
    
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    auto it = impl_->paths_.find(path);
    if (it == impl_->paths_.end()) {
        return false;
    }

    bool updated = false;
    if (is_key || it->second->config.vps.empty() || it->second->config.sps.empty() || it->second->config.pps.empty()) {
        updated = autoExtractH265ParameterSets(it->second->config, data, size);
    }
    if (updated) {
        RTSP_LOG_INFO("Auto-updated H265 parameter sets for path: " + path);
    }

    it->second->broadcastFrame(frame);
    impl_->stats_.frames_pushed++;
    return true;
}

std::shared_ptr<IVideoFrameInput> RtspServer::getFrameInput(const std::string& path) {
    class FrameInput : public IVideoFrameInput {
    public:
        FrameInput(RtspServer::Impl* impl, std::string p) : impl_(impl), path_(std::move(p)) {}
        bool pushFrame(const VideoFrame& frame) override {
            std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
            auto it = impl_->paths_.find(path_);
            if (it == impl_->paths_.end()) {
                return false;
            }
            it->second->broadcastFrame(frame);
            return true;
        }
    private:
        RtspServer::Impl* impl_;
        std::string path_;
    };
    return std::make_shared<FrameInput>(impl_.get(), path);
}

void RtspServer::setClientConnectCallback(ClientConnectCallback callback) {
    impl_->connect_callback_ = callback;
}

void RtspServer::setClientDisconnectCallback(ClientDisconnectCallback callback) {
    impl_->disconnect_callback_ = callback;
}

void RtspServer::setAuth(const std::string& username, const std::string& password,
                         const std::string& realm) {
    impl_->config_.auth_enabled = true;
    impl_->config_.auth_use_digest = false;
    impl_->config_.auth_username = username;
    impl_->config_.auth_password = password;
    impl_->config_.auth_realm = realm;
    if (impl_->config_.auth_nonce.empty()) {
        impl_->config_.auth_nonce = "nonce-" + generateNonce();
    }
}

void RtspServer::setAuthDigest(const std::string& username, const std::string& password,
                               const std::string& realm) {
    impl_->config_.auth_enabled = true;
    impl_->config_.auth_use_digest = true;
    impl_->config_.auth_username = username;
    impl_->config_.auth_password = password;
    impl_->config_.auth_realm = realm;
    if (impl_->config_.auth_nonce.empty()) {
        impl_->config_.auth_nonce = "nonce-" + generateNonce();
    }
}

RtspServerStats RtspServer::getStats() const {
    RtspServerStats s;
    s.requests_total = impl_->stats_.requests_total.load();
    s.auth_challenges = impl_->stats_.auth_challenges.load();
    s.auth_failures = impl_->stats_.auth_failures.load();
    s.sessions_created = impl_->stats_.sessions_created.load();
    s.sessions_closed = impl_->stats_.sessions_closed.load();
    s.frames_pushed = impl_->stats_.frames_pushed.load();
    s.rtp_packets_sent = impl_->stats_.rtp_packets_sent.load();
    s.rtp_bytes_sent = impl_->stats_.rtp_bytes_sent.load();
    return s;
}

// 工具函数
VideoFrame createVideoFrame(CodecType codec, const uint8_t* data, size_t size,
                            uint64_t pts, uint32_t width, uint32_t height, uint32_t fps) {
    VideoFrame frame = {};
    frame.codec = codec;
    frame.type = FrameType::P;
    frame.managed_data = makeManagedBuffer(data, size);
    frame.data = frame.managed_data->empty() ? nullptr : frame.managed_data->data();
    frame.size = frame.managed_data->size();
    frame.pts = pts;
    frame.dts = pts;
    frame.width = width;
    frame.height = height;
    frame.fps = fps;
    return frame;
}

void freeVideoFrame(VideoFrame& frame) {
    if (!frame.managed_data && frame.data) {
        delete[] frame.data;
    }
    frame.managed_data.reset();
    frame.data = nullptr;
    frame.size = 0;
}

std::shared_ptr<RtspServer> getOrCreateRtspServer(uint16_t port, const std::string& host) {
    auto& registry = globalServerRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);

    auto it = registry.instances.find(port);
    if (it != registry.instances.end()) {
        if (auto existing = it->second.lock()) {
            auto h = registry.first_host.find(port);
            if (h != registry.first_host.end() && h->second != host) {
                RTSP_LOG_WARNING("getOrCreateRtspServer reused existing server on port " +
                                 std::to_string(port) + ", keep first host=" + h->second);
            }
            return existing;
        }
        registry.instances.erase(it);
    }

    auto server = std::make_shared<RtspServer>();
    server->init(host, port);
    registry.instances[port] = server;
    registry.first_host[port] = host;
    return server;
}

} // namespace rtsp
