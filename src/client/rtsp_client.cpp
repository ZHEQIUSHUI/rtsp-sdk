#include <rtsp-client/rtsp_client.h>
#include <rtsp-common/socket.h>
#include <rtsp-common/rtsp_request.h>
#include <rtsp-common/sdp.h>
#include <rtsp-common/common.h>
#include <cstring>
#include <sstream>
#include <thread>
#include <queue>
#include <regex>
#include <map>
#include <unordered_map>
#include <iomanip>
#include <future>

namespace rtsp {

namespace {

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

void releaseFrameData(VideoFrame& frame) {
    frame.managed_data.reset();
    frame.data = nullptr;
    frame.size = 0;
}

bool joinThreadWithTimeout(std::thread& t, uint32_t timeout_ms) {
    if (!t.joinable()) {
        return true;
    }

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

} // namespace

// RTP接收器实现（简化版）
class RtpReceiver {
public:
    struct StatsSnapshot {
        uint64_t packets_received = 0;
        uint64_t packets_reordered = 0;
        uint64_t packet_loss_events = 0;
        uint64_t frames_output = 0;
    };

    RtpReceiver() = default;
    ~RtpReceiver() {
        stop();
    }

    bool init(uint16_t rtp_port, uint16_t rtcp_port) {
        if (!rtp_socket_.bindUdp("0.0.0.0", rtp_port)) {
            return false;
        }
        if (!rtcp_socket_.bindUdp("0.0.0.0", rtcp_port)) {
            return false;
        }
        rtp_socket_.setNonBlocking(true);
        rtcp_socket_.setNonBlocking(true);
        rtp_port_ = rtp_port;
        rtcp_port_ = rtcp_port;
        return true;
    }

    void start() {
        if (running_) return;
        running_ = true;
        receive_thread_ = std::thread([this]() { receiveLoop(); });
    }

    void stop() {
        if (!running_ && !receive_thread_.joinable()) {
            return;
        }
        running_ = false;
        if (rtp_port_ != 0) {
            // Wake a potentially blocking recvFrom on some platforms.
            Socket wake_socket;
            if (wake_socket.bindUdp("0.0.0.0", 0)) {
                uint8_t b = 0;
                wake_socket.sendTo(&b, 1, "127.0.0.1", rtp_port_);
            }
        }
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        rtp_socket_.close();
        rtcp_socket_.close();
    }

    void setCallback(FrameCallback callback) {
        callback_ = callback;
    }

    void setVideoInfo(CodecType codec, uint32_t width, uint32_t height, uint32_t fps, uint8_t payload_type) {
        codec_ = codec;
        width_ = width;
        height_ = height;
        fps_ = fps;
        payload_type_ = payload_type;
    }

    void setJitterBufferPackets(uint32_t packets) {
        jitter_buffer_packets_ = packets == 0 ? 1 : packets;
    }

    StatsSnapshot getStats() const {
        StatsSnapshot s;
        s.packets_received = packets_received_.load();
        s.packets_reordered = packets_reordered_.load();
        s.packet_loss_events = packet_loss_events_.load();
        s.frames_output = frames_output_.load();
        return s;
    }

    void ingestRtpPacket(const uint8_t* data, size_t len) {
        if (!data || len < 12) return;
        uint16_t seq = static_cast<uint16_t>((data[2] << 8) | data[3]);
        packets_received_++;

        if (!reorder_initialized_) {
            expected_seq_ = seq;
            reorder_initialized_ = true;
        }
        if (seq != expected_seq_) {
            packets_reordered_++;
        }

        reorder_buffer_[seq] = std::vector<uint8_t>(data, data + len);

        while (true) {
            auto it = reorder_buffer_.find(expected_seq_);
            if (it == reorder_buffer_.end()) break;
            processRtpPacket(it->second.data(), it->second.size());
            reorder_buffer_.erase(it);
            expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);
        }

        if (reorder_buffer_.size() > jitter_buffer_packets_) {
            auto it = reorder_buffer_.begin();
            expected_seq_ = it->first;
            while (true) {
                auto run = reorder_buffer_.find(expected_seq_);
                if (run == reorder_buffer_.end()) break;
                processRtpPacket(run->second.data(), run->second.size());
                reorder_buffer_.erase(run);
                expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);
            }
        }
    }

    uint16_t getRtpPort() const { return rtp_port_; }
    uint16_t getRtcpPort() const { return rtcp_port_; }

private:
    void appendAnnexBNalu(const uint8_t* nalu, size_t len) {
        if (!nalu || len == 0) return;
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
        if (frame_buffer_.empty()) return;

        VideoFrame frame;
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
        frames_output_++;
        frame_buffer_.clear();
        frame_is_idr_ = false;
        frame_in_progress_ = false;
    }

    void receiveLoop() {
        uint8_t buffer[65536];
        std::string from_ip;
        uint16_t from_port;

        while (running_) {
            ssize_t len = rtp_socket_.recvFrom(buffer, sizeof(buffer), from_ip, from_port);
            if (len > 0) {
                ingestRtpPacket(buffer, static_cast<size_t>(len));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void processRtpPacket(const uint8_t* data, size_t len) {
        if (len < 12) return;

        // 解析RTP头
        uint8_t version = (data[0] >> 6) & 0x03;
        if (version != 2) return;

        bool marker = (data[1] >> 7) & 0x01;
        uint8_t payload_type = data[1] & 0x7F;
        uint16_t seq = static_cast<uint16_t>((data[2] << 8) | data[3]);
        uint32_t timestamp = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];

        if (seq_initialized_) {
            uint16_t expected = static_cast<uint16_t>(last_seq_ + 1);
            if (seq != expected) {
                // H.265 FU packet loss: discard current FU assembly and wait for next FU start.
                if (codec_ == CodecType::H265 && h265_fu_in_progress_) {
                    packet_loss_events_++;
                    h265_fu_drop_mode_ = true;
                    h265_fu_in_progress_ = false;
                    if (h265_fu_start_offset_ <= frame_buffer_.size()) {
                        frame_buffer_.resize(h265_fu_start_offset_);
                    } else {
                        frame_buffer_.clear();
                    }
                }
            }
        }
        seq_initialized_ = true;
        last_seq_ = seq;

        // 解析可变RTP头（CSRC/extension/padding）
        uint8_t cc = data[0] & 0x0F;
        bool extension = (data[0] & 0x10) != 0;
        bool padding = (data[0] & 0x20) != 0;
        size_t header_len = 12 + static_cast<size_t>(cc) * 4;
        if (header_len > len) return;
        if (extension) {
            if (header_len + 4 > len) return;
            uint16_t ext_words = static_cast<uint16_t>((data[header_len + 2] << 8) | data[header_len + 3]);
            size_t ext_len = 4 + static_cast<size_t>(ext_words) * 4;
            if (header_len + ext_len > len) return;
            header_len += ext_len;
        }
        size_t payload_len = len - header_len;
        if (padding) {
            if (payload_len == 0) return;
            uint8_t pad_len = data[len - 1];
            if (pad_len == 0 || pad_len > payload_len) return;
            payload_len -= pad_len;
        }

        const uint8_t* payload = data + header_len;
        size_t payload_size = payload_len;
        if (payload_size == 0) return;

        // 处理H.264/H.265 payload（简化版）
        processVideoPayload(payload, payload_size, timestamp, marker, payload_type);
    }

    void processVideoPayload(const uint8_t* data, size_t len, uint32_t timestamp, 
                             bool marker, uint8_t payload_type) {
        if (len == 0) return;

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

        bool is_h264 = (payload_type == payload_type_ && codec_ == CodecType::H264);
        if (is_h264) {
            uint8_t nal_type = data[0] & 0x1F;
            if (nal_type >= 1 && nal_type <= 23) {
                appendAnnexBNalu(data, len);
                if (nal_type == 5) frame_is_idr_ = true;
            } else if (nal_type == 24) {
                // STAP-A: [STAP-A hdr][nalu_size(2)][nalu]...
                size_t off = 1;
                while (off + 2 <= len) {
                    uint16_t nalu_size = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
                    off += 2;
                    if (nalu_size == 0 || off + nalu_size > len) {
                        break;
                    }
                    uint8_t inner_type = data[off] & 0x1F;
                    appendAnnexBNalu(data + off, nalu_size);
                    if (inner_type == 5) frame_is_idr_ = true;
                    off += nalu_size;
                }
            } else if (nal_type == 25) {
                // STAP-B: [STAP-B hdr][DON(2)][nalu_size(2)][nalu]...
                if (len < 3) return;
                size_t off = 3;
                while (off + 2 <= len) {
                    uint16_t nalu_size = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
                    off += 2;
                    if (nalu_size == 0 || off + nalu_size > len) {
                        break;
                    }
                    uint8_t inner_type = data[off] & 0x1F;
                    appendAnnexBNalu(data + off, nalu_size);
                    if (inner_type == 5) frame_is_idr_ = true;
                    off += nalu_size;
                }
            } else if (nal_type == 28 && len >= 2) {
                uint8_t fu_header = data[1];
                bool start = (fu_header & 0x80) != 0;
                uint8_t reconstructed_nal = (data[0] & 0xE0) | (fu_header & 0x1F);
                if (start) {
                    static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
                    frame_buffer_.insert(frame_buffer_.end(), start_code, start_code + 4);
                    frame_buffer_.push_back(reconstructed_nal);
                    if ((reconstructed_nal & 0x1F) == 5) frame_is_idr_ = true;
                }
                if (len > 2) {
                    frame_buffer_.insert(frame_buffer_.end(), data + 2, data + len);
                }
            }
        } else {
            // H.265
            if (len < 2) return;
            uint8_t nal_type = (data[0] >> 1) & 0x3F;
            if (nal_type != 49 && nal_type != 48 && nal_type != 50) {
                appendAnnexBNalu(data, len);
                if (isH265Irap(nal_type)) frame_is_idr_ = true;
            } else if (nal_type == 48) {
                // AP: [payload hdr(2)][nalu_size(2)][nalu]...
                size_t off = 2;
                while (off + 2 <= len) {
                    uint16_t nalu_size = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
                    off += 2;
                    if (nalu_size == 0 || off + nalu_size > len) {
                        break;
                    }
                    uint8_t inner_type = (data[off] >> 1) & 0x3F;
                    appendAnnexBNalu(data + off, nalu_size);
                    if (isH265Irap(inner_type)) frame_is_idr_ = true;
                    off += nalu_size;
                }
            } else if (nal_type == 49 && len >= 3) {
                uint8_t fu_indicator0 = data[0];
                uint8_t fu_indicator1 = data[1];
                uint8_t fu_header = data[2];
                bool start = (fu_header & 0x80) != 0;
                bool end = (fu_header & 0x40) != 0;
                uint8_t orig_type = fu_header & 0x3F;
                uint8_t orig0 = (fu_indicator0 & 0x81) | (orig_type << 1);
                uint8_t orig1 = fu_indicator1;
                if (start) {
                    h265_fu_drop_mode_ = false;
                    h265_fu_in_progress_ = true;
                    h265_fu_start_offset_ = frame_buffer_.size();
                    static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
                    frame_buffer_.insert(frame_buffer_.end(), start_code, start_code + 4);
                    frame_buffer_.push_back(orig0);
                    frame_buffer_.push_back(orig1);
                    if (isH265Irap(orig_type)) frame_is_idr_ = true;
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

private:
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
    std::atomic<uint64_t> packets_received_{0};
    std::atomic<uint64_t> packets_reordered_{0};
    std::atomic<uint64_t> packet_loss_events_{0};
    std::atomic<uint64_t> frames_output_{0};
};

// RtspClient::Impl 定义
class RtspClient::Impl {
public:
    RtspClientConfig config_;
    std::unique_ptr<Socket> control_socket_;
    std::unique_ptr<RtpReceiver> rtp_receiver_;
    
    std::string server_url_;
    std::string request_url_;
    std::string server_host_;
    uint16_t server_port_ = 554;
    std::string server_path_;
    std::string auth_user_;
    std::string auth_pass_;
    std::string basic_auth_header_;
    bool use_digest_auth_ = false;
    std::string digest_realm_;
    std::string digest_nonce_;
    std::string digest_qop_ = "auth";
    uint32_t digest_nc_ = 0;
    bool use_tcp_transport_ = false;
    uint8_t interleaved_rtp_channel_ = 0;
    uint8_t interleaved_rtcp_channel_ = 1;

    SessionInfo session_info_;
    int cseq_ = 0;
    std::string session_id_;
    bool connected_ = false;
    bool playing_ = false;
    bool receiver_started_ = false;
    std::atomic<bool> stop_waiting_{false};
    std::thread tcp_receive_thread_;
    std::atomic<bool> tcp_receive_running_{false};
    std::atomic<uint64_t> auth_retries_{0};
    
    FrameCallback frame_callback_;
    ErrorCallback error_callback_;
    
    std::queue<VideoFrame> frame_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    bool parseUrl(const std::string& url) {
        if (url.find("rtsp://") != 0) {
            return false;
        }

        std::string no_scheme = url.substr(7);
        size_t at_pos = no_scheme.find('@');
        std::string host_part = no_scheme;
        if (at_pos != std::string::npos) {
            std::string userinfo = no_scheme.substr(0, at_pos);
            host_part = no_scheme.substr(at_pos + 1);
            size_t colon = userinfo.find(':');
            if (colon != std::string::npos) {
                auth_user_ = userinfo.substr(0, colon);
                auth_pass_ = userinfo.substr(colon + 1);
            } else {
                auth_user_ = userinfo;
                auth_pass_.clear();
            }
        }

        size_t slash_pos = host_part.find('/');
        std::string host_port = slash_pos == std::string::npos ? host_part : host_part.substr(0, slash_pos);
        server_path_ = slash_pos == std::string::npos ? "/" : host_part.substr(slash_pos);
        if (server_path_.empty()) server_path_ = "/";

        size_t colon_pos = host_port.find(':');
        if (colon_pos != std::string::npos) {
            server_host_ = host_port.substr(0, colon_pos);
            server_port_ = static_cast<uint16_t>(std::stoi(host_port.substr(colon_pos + 1)));
        } else {
            server_host_ = host_port;
            server_port_ = 554;
        }
        if (server_host_.empty()) return false;

        request_url_ = "rtsp://" + server_host_ + ":" + std::to_string(server_port_) + server_path_;
        server_url_ = request_url_;
        if (!auth_user_.empty()) {
            std::string token = auth_user_ + ":" + auth_pass_;
            basic_auth_header_ = "Basic " + base64Encode(reinterpret_cast<const uint8_t*>(token.data()), token.size());
        } else {
            basic_auth_header_.clear();
        }
        
        return true;
    }

    void appendCommonHeaders(std::ostringstream& request, const std::string& method = "",
                             const std::string& uri = "") {
        request << "User-Agent: " << config_.user_agent << "\r\n";
        if (use_digest_auth_ && !method.empty() && !uri.empty()) {
            std::string digest = buildDigestAuthorization(method, uri);
            if (!digest.empty()) {
                request << "Authorization: " << digest << "\r\n";
                return;
            }
        }
        if (!basic_auth_header_.empty()) {
            request << "Authorization: " << basic_auth_header_ << "\r\n";
        }
    }

    bool stopTcpReceiverThread(uint32_t timeout_ms) {
        tcp_receive_running_ = false;
        return joinThreadWithTimeout(tcp_receive_thread_, timeout_ms);
    }

    bool recvExact(uint8_t* out, size_t n, int timeout_ms) {
        size_t off = 0;
        while (off < n && tcp_receive_running_) {
            ssize_t r = control_socket_->recv(out + off, n - off, timeout_ms);
            if (r > 0) {
                off += static_cast<size_t>(r);
            } else if (r == 0) {
                continue;
            } else {
                return false;
            }
        }
        return off == n;
    }

    void tcpReceiveLoop() {
        while (tcp_receive_running_) {
            uint8_t b = 0;
            if (!recvExact(&b, 1, 200)) {
                continue;
            }
            if (b != '$') {
                continue;
            }
            uint8_t hdr[3];
            if (!recvExact(hdr, 3, 200)) {
                continue;
            }
            uint8_t channel = hdr[0];
            uint16_t size = static_cast<uint16_t>((hdr[1] << 8) | hdr[2]);
            if (size == 0) {
                continue;
            }
            std::vector<uint8_t> payload(size);
            if (!recvExact(payload.data(), payload.size(), 200)) {
                continue;
            }
            if (channel == interleaved_rtp_channel_ && rtp_receiver_) {
                rtp_receiver_->ingestRtpPacket(payload.data(), payload.size());
            }
        }
    }

    bool parseSdp(const std::string& sdp) {
        session_info_.media_streams.clear();
        
        std::istringstream stream(sdp);
        std::string line;
        MediaInfo* current_media = nullptr;
        
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            
            if (line.find("m=video") == 0) {
                session_info_.has_video = true;
                session_info_.media_streams.emplace_back();
                current_media = &session_info_.media_streams.back();
                
                std::istringstream iss(line);
                std::string type, port, proto, pt;
                iss >> type >> port >> proto >> pt;
                current_media->payload_type = std::stoi(pt);
            }
            else if (line.find("a=rtpmap:") == 0 && current_media) {
                std::regex rtpmap_regex("a=rtpmap:(\\d+)\\s+(\\w+)/(\\d+)");
                std::smatch match;
                if (std::regex_search(line, match, rtpmap_regex)) {
                    current_media->codec_name = match[2];
                    current_media->clock_rate = std::stoi(match[3]);
                    
                    if (current_media->codec_name.find("264") != std::string::npos) {
                        current_media->codec = CodecType::H264;
                    } else if (current_media->codec_name.find("265") != std::string::npos ||
                               current_media->codec_name.find("HEVC") != std::string::npos) {
                        current_media->codec = CodecType::H265;
                    }
                }
            }
            else if (line.find("a=control:") == 0 && current_media) {
                current_media->control_url = line.substr(10);
            }
            else if (line.find("a=framesize:") == 0 && current_media) {
                std::regex size_regex("a=framesize:(\\d+)\\s+(\\d+)-(\\d+)");
                std::smatch match;
                if (std::regex_search(line, match, size_regex)) {
                    current_media->width = std::stoi(match[2]);
                    current_media->height = std::stoi(match[3]);
                }
            }
            else if (line.find("a=cliprect:") == 0 && current_media) {
                std::regex clip_regex("a=cliprect:\\d+,\\d+,(\\d+),(\\d+)");
                std::smatch match;
                if (std::regex_search(line, match, clip_regex)) {
                    const uint32_t h = static_cast<uint32_t>(std::stoul(match[1].str()));
                    const uint32_t w = static_cast<uint32_t>(std::stoul(match[2].str()));
                    if (w > 0 && h > 0) {
                        current_media->width = w;
                        current_media->height = h;
                    }
                }
            }
            else if (line.find("a=framerate:") == 0 && current_media) {
                std::regex fr_regex("a=framerate:(\\d+(?:\\.\\d+)?)");
                std::smatch match;
                if (std::regex_search(line, match, fr_regex)) {
                    current_media->fps = static_cast<uint32_t>(std::stod(match[1]));
                }
            }
            else if (line.find("a=fmtp:") == 0 && current_media) {
                std::smatch match;
                std::regex h264_sprop("sprop-parameter-sets=([^;\\s]+)");
                if (std::regex_search(line, match, h264_sprop)) {
                    std::string sprops = match[1].str();
                    size_t comma = sprops.find(',');
                    if (comma != std::string::npos) {
                        current_media->sps = base64Decode(sprops.substr(0, comma));
                        current_media->pps = base64Decode(sprops.substr(comma + 1));
                    }
                }
                std::regex h265_vps("sprop-vps=([^;\\s]+)");
                if (std::regex_search(line, match, h265_vps)) {
                    current_media->vps = base64Decode(match[1].str());
                }
                std::regex h265_sps("sprop-sps=([^;\\s]+)");
                if (std::regex_search(line, match, h265_sps)) {
                    current_media->sps = base64Decode(match[1].str());
                }
                std::regex h265_pps("sprop-pps=([^;\\s]+)");
                if (std::regex_search(line, match, h265_pps)) {
                    current_media->pps = base64Decode(match[1].str());
                }
            }
        }

        for (auto& media : session_info_.media_streams) {
            if (media.width == 0) media.width = 1920;
            if (media.height == 0) media.height = 1080;
            if (media.fps == 0) media.fps = 30;
            if (media.clock_rate == 0) media.clock_rate = 90000;
            if (media.payload_type == 0) {
                media.payload_type = (media.codec == CodecType::H265) ? 97 : 96;
            }
        }
        
        return true;
    }

    bool sendRequest(const std::string& method, const std::string& uri,
                     const std::string& extra_headers, const std::string& body,
                     std::string& response, bool allow_retry_401 = true) {
        auto send_once = [&](bool with_auth) -> bool {
            std::ostringstream req;
            req << method << " " << uri << " RTSP/1.0\r\n";
            req << "CSeq: " << ++cseq_ << "\r\n";
            if (!extra_headers.empty()) req << extra_headers;
            if (!body.empty()) {
                req << "Content-Length: " << body.size() << "\r\n";
            }
            if (with_auth) {
                appendCommonHeaders(req, method, uri);
            } else {
                req << "User-Agent: " << config_.user_agent << "\r\n";
            }
            req << "\r\n";
            if (!body.empty()) req << body;

            std::string req_str = req.str();
            control_socket_->send((const uint8_t*)req_str.c_str(), req_str.size());
            char buffer[8192];
            ssize_t len = control_socket_->recv((uint8_t*)buffer, sizeof(buffer), 5000);
            if (len <= 0) return false;
            response.assign(buffer, static_cast<size_t>(len));
            return true;
        };

        if (!send_once(true)) return false;
        if (response.find("401") != std::string::npos && allow_retry_401 && !auth_user_.empty()) {
            if (parseWwwAuthenticate(response)) {
                auth_retries_++;
                return send_once(true);
            }
        }
        return true;
    }

    static std::unordered_map<std::string, std::string> parseAuthParams(const std::string& value) {
        std::unordered_map<std::string, std::string> kv;
        size_t pos = 0;
        while (pos < value.size()) {
            while (pos < value.size() && (value[pos] == ' ' || value[pos] == ',')) pos++;
            size_t eq = value.find('=', pos);
            if (eq == std::string::npos) break;
            std::string key = value.substr(pos, eq - pos);
            pos = eq + 1;
            std::string v;
            if (pos < value.size() && value[pos] == '"') {
                size_t endq = value.find('"', pos + 1);
                if (endq == std::string::npos) break;
                v = value.substr(pos + 1, endq - pos - 1);
                pos = endq + 1;
            } else {
                size_t comma = value.find(',', pos);
                if (comma == std::string::npos) {
                    v = value.substr(pos);
                    pos = value.size();
                } else {
                    v = value.substr(pos, comma - pos);
                    pos = comma + 1;
                }
            }
            kv[key] = v;
        }
        return kv;
    }

    bool parseWwwAuthenticate(const std::string& response) {
        std::regex ww_re("WWW-Authenticate:\\s*([^\\r\\n]+)", std::regex::icase);
        std::smatch m;
        if (!std::regex_search(response, m, ww_re)) {
            return false;
        }
        std::string challenge = m[1].str();
        if (challenge.rfind("Digest ", 0) == 0) {
            auto p = parseAuthParams(challenge.substr(7));
            digest_realm_ = p["realm"];
            digest_nonce_ = p["nonce"];
            if (!p["qop"].empty()) digest_qop_ = p["qop"];
            use_digest_auth_ = !digest_realm_.empty() && !digest_nonce_.empty();
            return use_digest_auth_;
        }
        if (challenge.rfind("Basic ", 0) == 0) {
            use_digest_auth_ = false;
            return !basic_auth_header_.empty();
        }
        return false;
    }

    std::string buildDigestAuthorization(const std::string& method, const std::string& uri) {
        if (auth_user_.empty() || auth_pass_.empty() || digest_nonce_.empty() || digest_realm_.empty()) {
            return "";
        }
        digest_nc_++;
        std::stringstream nc_ss;
        nc_ss << std::hex << std::setw(8) << std::setfill('0') << digest_nc_;
        std::string nc = nc_ss.str();
        std::string cnonce = md5Hex(std::to_string(digest_nc_) + ":" + auth_user_ + ":" + uri).substr(0, 16);
        std::string ha1 = md5Hex(auth_user_ + ":" + digest_realm_ + ":" + auth_pass_);
        std::string ha2 = md5Hex(method + ":" + uri);
        std::string response = md5Hex(ha1 + ":" + digest_nonce_ + ":" + nc + ":" + cnonce + ":" + digest_qop_ + ":" + ha2);

        std::ostringstream out;
        out << "Digest username=\"" << auth_user_ << "\", realm=\"" << digest_realm_
            << "\", nonce=\"" << digest_nonce_ << "\", uri=\"" << uri
            << "\", response=\"" << response << "\", qop=" << digest_qop_
            << ", nc=" << nc << ", cnonce=\"" << cnonce << "\"";
        return out.str();
    }

    void onFrame(const VideoFrame& frame) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (frame_queue_.size() >= config_.buffer_size) {
                auto& old = frame_queue_.front();
                releaseFrameData(old);
                frame_queue_.pop();
            }
            
            VideoFrame copy = cloneFrameManaged(frame);
            frame_queue_.push(copy);
        }
        queue_cv_.notify_one();

        if (frame_callback_) {
            frame_callback_(frame);
        }
    }
};

// RtspClient 实现
RtspClient::RtspClient() : impl_(std::make_unique<Impl>()) {}

RtspClient::~RtspClient() {
    close();
}

void RtspClient::setConfig(const RtspClientConfig& config) {
    impl_->config_ = config;
}

bool RtspClient::open(const std::string& url) {
    if (!impl_->parseUrl(url)) {
        return false;
    }

    impl_->control_socket_ = std::make_unique<Socket>();
    if (!impl_->control_socket_->connect(impl_->server_host_, impl_->server_port_, 10000)) {
        return false;
    }

    impl_->connected_ = true;
    impl_->stop_waiting_ = false;
    return true;
}

bool RtspClient::describe() {
    if (!impl_->connected_) return false;
    std::string response;
    if (!impl_->sendRequest("DESCRIBE", impl_->request_url_,
                            "Accept: application/sdp\r\n", "", response)) {
        return false;
    }
    
    if (response.find("200 OK") == std::string::npos) {
        return false;
    }

    size_t sdp_start = response.find("\r\n\r\n");
    if (sdp_start == std::string::npos) return false;
    
    std::string sdp = response.substr(sdp_start + 4);
    return impl_->parseSdp(sdp);
}

SessionInfo RtspClient::getSessionInfo() const {
    return impl_->session_info_;
}

void RtspClient::setFrameCallback(FrameCallback callback) {
    impl_->frame_callback_ = callback;
}

void RtspClient::setErrorCallback(ErrorCallback callback) {
    impl_->error_callback_ = callback;
}

bool RtspClient::setup(int stream_index) {
    if (!impl_->connected_) return false;
    if (stream_index >= (int)impl_->session_info_.media_streams.size()) return false;

    auto& media = impl_->session_info_.media_streams[stream_index];
    
    std::string control_url = media.control_url;
    if (control_url.find("rtsp://") != 0) {
        control_url = impl_->request_url_ + "/" + control_url;
    }

    auto do_setup = [&](bool use_tcp, std::string& response_out) -> bool {
        uint16_t selected_rtp_port = 0;
        if (use_tcp) {
            impl_->rtp_receiver_ = std::make_unique<RtpReceiver>();
            impl_->rtp_receiver_->setJitterBufferPackets(impl_->config_.jitter_buffer_packets);
        } else {
            bool bound = false;
            uint32_t start = impl_->config_.rtp_port_start;
            uint32_t end = impl_->config_.rtp_port_end;
            if (end <= start + 1) {
                end = start + 64;
            }
            for (uint32_t p = start; p + 1 < end; p += 2) {
                auto candidate = std::make_unique<RtpReceiver>();
                candidate->setJitterBufferPackets(impl_->config_.jitter_buffer_packets);
                if (candidate->init(static_cast<uint16_t>(p), static_cast<uint16_t>(p + 1))) {
                    selected_rtp_port = static_cast<uint16_t>(p);
                    impl_->rtp_receiver_ = std::move(candidate);
                    bound = true;
                    break;
                }
            }
            if (!bound) {
                return false;
            }
        }

        std::ostringstream request;
        request.clear();
        if (use_tcp) {
            request << "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
        } else {
            request << "Transport: RTP/AVP;unicast;client_port="
                    << selected_rtp_port << "-" << (selected_rtp_port + 1) << "\r\n";
        }
        if (!impl_->sendRequest("SETUP", control_url, request.str(), "", response_out)) {
            return false;
        }
        if (response_out.find("200 OK") == std::string::npos) {
            return false;
        }
        return true;
    };

    std::string response;
    bool use_tcp = impl_->config_.prefer_tcp_transport;
    bool ok = do_setup(use_tcp, response);
    if (!ok && impl_->config_.fallback_to_tcp) {
        if (!use_tcp && response.find("461") != std::string::npos) {
            use_tcp = true;
            ok = do_setup(use_tcp, response);
        } else if (use_tcp && (response.find("461") != std::string::npos || response.find("400") != std::string::npos)) {
            use_tcp = false;
            ok = do_setup(use_tcp, response);
        }
    }
    if (!ok) return false;
    impl_->use_tcp_transport_ = use_tcp;

    std::regex session_regex("Session:\\s*([^;\\r\\n]+)");
    std::smatch match;
    if (std::regex_search(response, match, session_regex)) {
        impl_->session_id_ = match[1];
    }
    if (impl_->use_tcp_transport_) {
        std::regex interleaved_regex("interleaved=(\\d+)-(\\d+)", std::regex::icase);
        std::smatch tm;
        if (std::regex_search(response, tm, interleaved_regex)) {
            impl_->interleaved_rtp_channel_ = static_cast<uint8_t>(std::stoi(tm[1].str()));
            impl_->interleaved_rtcp_channel_ = static_cast<uint8_t>(std::stoi(tm[2].str()));
        }
    }

    impl_->rtp_receiver_->setVideoInfo(media.codec, media.width, media.height, media.fps, static_cast<uint8_t>(media.payload_type));
    impl_->rtp_receiver_->setCallback([this](const VideoFrame& frame) {
        impl_->onFrame(frame);
    });

    return true;
}

bool RtspClient::play(uint64_t start_time_ms) {
    if (!impl_->connected_ || impl_->session_id_.empty()) return false;

    std::ostringstream extra;
    extra << "Session: " << impl_->session_id_ << "\r\n";
    if (start_time_ms > 0) {
        extra << "Range: npt=" << (start_time_ms / 1000.0) << "-\r\n";
    } else {
        extra << "Range: npt=0.000-\r\n";
    }
    std::string response;
    if (!impl_->sendRequest("PLAY", impl_->request_url_, extra.str(), "", response)) {
        return false;
    }
    if (response.find("200 OK") == std::string::npos) {
        return false;
    }

    impl_->playing_ = true;
    impl_->stop_waiting_ = false;
    if (impl_->rtp_receiver_ && !impl_->receiver_started_) {
        if (impl_->use_tcp_transport_) {
            impl_->tcp_receive_running_ = true;
            impl_->tcp_receive_thread_ = std::thread([this]() {
                impl_->tcpReceiveLoop();
            });
        } else {
            impl_->rtp_receiver_->start();
        }
        impl_->receiver_started_ = true;
    }
    
    return true;
}

bool RtspClient::pause() {
    if (!impl_->connected_ || impl_->session_id_.empty()) return false;

    if (impl_->use_tcp_transport_ && impl_->receiver_started_) {
        impl_->stopTcpReceiverThread(1000);
        impl_->receiver_started_ = false;
    }

    std::ostringstream extra;
    extra << "Session: " << impl_->session_id_ << "\r\n";
    std::string response;
    bool ok = impl_->sendRequest("PAUSE", impl_->request_url_, extra.str(), "", response);
    
    impl_->playing_ = false;
    impl_->stop_waiting_ = true;
    impl_->queue_cv_.notify_all();
    if (!impl_->use_tcp_transport_ && impl_->rtp_receiver_ && impl_->receiver_started_) {
        impl_->rtp_receiver_->stop();
        impl_->receiver_started_ = false;
    }
    return ok && response.find("200 OK") != std::string::npos;
}

bool RtspClient::teardown() {
    if (!impl_->connected_ || impl_->session_id_.empty()) return false;

    if (impl_->use_tcp_transport_ && impl_->receiver_started_) {
        impl_->stopTcpReceiverThread(1000);
        impl_->receiver_started_ = false;
    }

    std::ostringstream extra;
    extra << "Session: " << impl_->session_id_ << "\r\n";
    std::string response;
    impl_->sendRequest("TEARDOWN", impl_->request_url_, extra.str(), "", response, false);

    impl_->playing_ = false;
    impl_->stop_waiting_ = true;
    impl_->queue_cv_.notify_all();
    impl_->session_id_.clear();
    
    if (impl_->rtp_receiver_) {
        impl_->rtp_receiver_->stop();
        impl_->receiver_started_ = false;
    }
    
    return true;
}

void RtspClient::receiveLoop() {
    std::unique_lock<std::mutex> lock(impl_->queue_mutex_);
    while (!impl_->stop_waiting_ && impl_->playing_) {
        impl_->queue_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
            return impl_->stop_waiting_ || !impl_->playing_ || !impl_->frame_queue_.empty();
        });
    }
}

bool RtspClient::receiveFrame(VideoFrame& frame, int timeout_ms) {
    std::unique_lock<std::mutex> lock(impl_->queue_mutex_);
    
    if (!impl_->queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this] { return !impl_->frame_queue_.empty() || !impl_->playing_ || impl_->stop_waiting_; })) {
        return false;
    }

    if (impl_->frame_queue_.empty()) return false;

    frame = impl_->frame_queue_.front();
    impl_->frame_queue_.pop();
    return true;
}

bool RtspClient::isConnected() const {
    return impl_->connected_;
}

bool RtspClient::isPlaying() const {
    return impl_->playing_;
}

void RtspClient::close() {
    closeWithTimeout(5000);
}

bool RtspClient::closeWithTimeout(uint32_t timeout_ms) {
    teardown();
    if (impl_->use_tcp_transport_ && impl_->receiver_started_) {
        impl_->stopTcpReceiverThread(timeout_ms);
        impl_->receiver_started_ = false;
    }
    
    if (impl_->control_socket_) {
        impl_->control_socket_->close();
    }
    
    impl_->connected_ = false;
    impl_->playing_ = false;
    impl_->receiver_started_ = false;
    impl_->stop_waiting_ = true;
    {
        std::lock_guard<std::mutex> lock(impl_->queue_mutex_);
        while (!impl_->frame_queue_.empty()) {
            auto& f = impl_->frame_queue_.front();
            releaseFrameData(f);
            impl_->frame_queue_.pop();
        }
    }
    impl_->queue_cv_.notify_all();
    return true;
}

void RtspClient::interrupt() {
    impl_->stop_waiting_ = true;
    impl_->queue_cv_.notify_all();
}

bool RtspClient::sendOptions() {
    if (!impl_->connected_) return false;
    std::string response;
    if (!impl_->sendRequest("OPTIONS", impl_->request_url_, "", "", response)) return false;
    return response.find("200 OK") != std::string::npos;
}

bool RtspClient::sendGetParameter(const std::string& param) {
    if (!impl_->connected_ || impl_->session_id_.empty()) return false;
    bool restart_tcp_receiver = false;
    if (impl_->use_tcp_transport_ && impl_->receiver_started_) {
        impl_->stopTcpReceiverThread(1000);
        impl_->receiver_started_ = false;
        restart_tcp_receiver = impl_->playing_;
    }

    std::ostringstream extra;
    extra << "Session: " << impl_->session_id_ << "\r\n";
    extra << "Content-Type: text/parameters\r\n";
    std::string response;
    bool ok = impl_->sendRequest("GET_PARAMETER", impl_->request_url_, extra.str(), param, response);
    if (restart_tcp_receiver && impl_->rtp_receiver_) {
        impl_->tcp_receive_running_ = true;
        impl_->tcp_receive_thread_ = std::thread([this]() {
            impl_->tcpReceiveLoop();
        });
        impl_->receiver_started_ = true;
    }
    return ok && response.find("200 OK") != std::string::npos;
}

RtspClientStats RtspClient::getStats() const {
    RtspClientStats s;
    s.auth_retries = impl_->auth_retries_.load();
    s.using_tcp_transport = impl_->use_tcp_transport_;
    if (impl_->rtp_receiver_) {
        auto rs = impl_->rtp_receiver_->getStats();
        s.rtp_packets_received = rs.packets_received;
        s.rtp_packets_reordered = rs.packets_reordered;
        s.rtp_packet_loss_events = rs.packet_loss_events;
        s.frames_output = rs.frames_output;
    }
    return s;
}

// SimpleRtspPlayer 实现
SimpleRtspPlayer::SimpleRtspPlayer() = default;

SimpleRtspPlayer::~SimpleRtspPlayer() {
    close();
}

void SimpleRtspPlayer::setFrameCallback(FrameCallback callback) {
    frame_callback_ = callback;
}

void SimpleRtspPlayer::setErrorCallback(ErrorCallback callback) {
    error_callback_ = callback;
}

bool SimpleRtspPlayer::open(const std::string& url) {
    client_ = std::make_unique<RtspClient>();
    
    // 设置错误回调
    if (error_callback_) {
        client_->setErrorCallback(error_callback_);
    }
    
    if (!client_->open(url)) {
        if (error_callback_) {
            error_callback_("Failed to connect to: " + url);
        }
        return false;
    }
    
    if (!client_->describe()) {
        if (error_callback_) {
            error_callback_("DESCRIBE failed");
        }
        return false;
    }
    
    if (!client_->setup(0)) {
        if (error_callback_) {
            error_callback_("SETUP failed");
        }
        return false;
    }
    
    // 设置帧回调 - 同时支持外部回调和内部缓存
    client_->setFrameCallback([this](const VideoFrame& frame) {
        // 1. 如果设置了外部回调，先调用外部回调
        if (frame_callback_) {
            frame_callback_(frame);
        }
        
        // 2. 同时缓存到内部缓冲区，供 readFrame() 使用
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (frame_buffer_.size() < MAX_BUFFER_SIZE) {
                VideoFrame copy = cloneFrameManaged(frame);
                frame_buffer_.push_back(copy);
            }
            buffer_cv_.notify_one();
        }
    });

    running_ = true;
    receive_thread_ = std::thread([this]() {
        if (!client_->play(0)) {
            if (error_callback_) {
                error_callback_("PLAY failed");
            }
            running_ = false;
            return;
        }
        client_->receiveLoop();
        running_ = false;
        buffer_cv_.notify_all();
    });

    return true;
}

bool SimpleRtspPlayer::readFrame(VideoFrame& frame) {
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    
    buffer_cv_.wait(lock, [this] { 
        return !frame_buffer_.empty() || !running_; 
    });
    
    if (frame_buffer_.empty()) return false;
    
    frame = frame_buffer_.front();
    frame_buffer_.erase(frame_buffer_.begin());
    return true;
}

void SimpleRtspPlayer::close() {
    closeWithTimeout(5000);
}

bool SimpleRtspPlayer::closeWithTimeout(uint32_t timeout_ms) {
    running_ = false;
    buffer_cv_.notify_all();
    
    bool joined = joinThreadWithTimeout(receive_thread_, timeout_ms);
    
    if (client_) {
        client_->closeWithTimeout(timeout_ms);
    }
    
    for (auto& f : frame_buffer_) {
        releaseFrameData(f);
    }
    frame_buffer_.clear();
    return joined;
}

bool SimpleRtspPlayer::isRunning() const {
    return running_;
}

bool SimpleRtspPlayer::getMediaInfo(uint32_t& width, uint32_t& height, uint32_t& fps, CodecType& codec) {
    if (!client_) return false;
    
    auto info = client_->getSessionInfo();
    if (info.media_streams.empty()) return false;
    
    auto& media = info.media_streams[0];
    width = media.width;
    height = media.height;
    fps = media.fps;
    codec = media.codec;
    
    return true;
}

} // namespace rtsp
