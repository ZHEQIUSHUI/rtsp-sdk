#include <rtsp-publisher/rtsp_publisher.h>
#include <rtsp-common/socket.h>
#include <rtsp-common/sdp.h>
#include <rtsp-common/rtp_packer.h>
#include <rtsp-common/common.h>

#include <regex>
#include <sstream>
#include <random>

namespace rtsp {

namespace {

std::shared_ptr<std::vector<uint8_t>> makeManagedBuffer(const uint8_t* data, size_t size) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    if (data && size > 0) {
        buf->assign(data, data + size);
    }
    return buf;
}

} // namespace

class RtspPublisher::Impl {
public:
    RtspPublishConfig config_;
    std::unique_ptr<Socket> control_socket_;
    std::unique_ptr<RtpSender> rtp_sender_;
    std::unique_ptr<RtpPacker> rtp_packer_;
    PublishMediaInfo media_;

    std::string host_;
    uint16_t port_ = 554;
    std::string path_;
    std::string request_url_;
    std::string session_id_;
    uint16_t server_rtp_port_ = 0;
    uint16_t server_rtcp_port_ = 0;
    int cseq_ = 0;
    bool connected_ = false;
    bool announced_ = false;
    bool setup_done_ = false;
    bool recording_ = false;

    bool parseUrl(const std::string& url) {
        if (url.find("rtsp://") != 0) return false;
        std::string no_scheme = url.substr(7);
        size_t slash = no_scheme.find('/');
        std::string host_port = (slash == std::string::npos) ? no_scheme : no_scheme.substr(0, slash);
        path_ = (slash == std::string::npos) ? "/" : no_scheme.substr(slash);
        if (path_.empty()) path_ = "/";

        size_t colon = host_port.find(':');
        if (colon != std::string::npos) {
            host_ = host_port.substr(0, colon);
            uint32_t p = 0;
            if (!parseUint32Safe(host_port.substr(colon + 1), p) || p == 0 || p > 65535) {
                return false;
            }
            port_ = static_cast<uint16_t>(p);
        } else {
            host_ = host_port;
            port_ = 554;
        }
        if (host_.empty()) return false;
        request_url_ = "rtsp://" + host_ + ":" + std::to_string(port_) + path_;
        return true;
    }

    bool sendRequest(const std::string& method, const std::string& uri,
                     const std::string& headers, const std::string& body,
                     std::string& response, int recv_timeout_ms = 5000) {
        if (!control_socket_) return false;
        std::ostringstream req;
        req << method << " " << uri << " RTSP/1.0\r\n";
        req << "CSeq: " << ++cseq_ << "\r\n";
        req << "User-Agent: " << config_.user_agent << "\r\n";
        if (!session_id_.empty()) {
            req << "Session: " << session_id_ << "\r\n";
        }
        if (!headers.empty()) req << headers;
        if (!body.empty()) req << "Content-Length: " << body.size() << "\r\n";
        req << "\r\n";
        if (!body.empty()) req << body;

        const std::string wire = req.str();
        // 写也给超时保护，避免远端不读时阻塞
        ssize_t sent = control_socket_->sendAll(
            reinterpret_cast<const uint8_t*>(wire.data()), wire.size(),
            recv_timeout_ms > 0 ? recv_timeout_ms : 5000);
        if (sent != static_cast<ssize_t>(wire.size())) {
            return false;
        }

        // 用公共的 recvRtspMessage 替换单次 recv，处理 TCP 分片
        return recvRtspMessage(*control_socket_, &response, recv_timeout_ms);
    }

    bool parseSessionAndPorts(const std::string& response) {
        std::smatch m;
        static const std::regex session_regex("Session:\\s*([^;\\r\\n]+)", std::regex::icase);
        if (std::regex_search(response, m, session_regex)) {
            session_id_ = m[1].str();
        }
        static const std::regex server_port_regex("server_port=(\\d+)-(\\d+)", std::regex::icase);
        if (std::regex_search(response, m, server_port_regex)) {
            uint32_t rtp_p = 0, rtcp_p = 0;
            if (parseUint32Safe(m[1].str(), rtp_p) && rtp_p > 0 && rtp_p <= 65535) {
                server_rtp_port_ = static_cast<uint16_t>(rtp_p);
            }
            if (parseUint32Safe(m[2].str(), rtcp_p) && rtcp_p > 0 && rtcp_p <= 65535) {
                server_rtcp_port_ = static_cast<uint16_t>(rtcp_p);
            }
        }
        return !session_id_.empty();
    }
};

RtspPublisher::RtspPublisher() : impl_(std::make_unique<Impl>()) {}
RtspPublisher::~RtspPublisher() { close(); }

void RtspPublisher::setConfig(const RtspPublishConfig& config) {
    impl_->config_ = config;
}

bool RtspPublisher::open(const std::string& url) {
    if (!impl_->parseUrl(url)) return false;
    impl_->control_socket_ = std::make_unique<Socket>();
    if (!impl_->control_socket_->connect(impl_->host_, impl_->port_, 10000)) {
        return false;
    }
    impl_->connected_ = true;
    return true;
}

bool RtspPublisher::announce(const PublishMediaInfo& media) {
    if (!impl_->connected_) return false;
    impl_->media_ = media;

    SdpBuilder sdp;
    // 部分严格 RTSP 服务器会拒绝 c=IN IP4 0.0.0.0；优先用实际本地 IP
    std::string conn_ip;
    if (impl_->control_socket_) {
        conn_ip = impl_->control_socket_->getLocalIp();
    }
    if (conn_ip.empty() || conn_ip == "0.0.0.0") {
        conn_ip = "127.0.0.1";
    }
    sdp.setConnection("IN", "IP4", conn_ip);
    const uint32_t clock_rate = 90000;
    const std::string control = media.control_track.empty() ? "streamid=0" : media.control_track;
    if (media.codec == CodecType::H264) {
        const std::string sps_b64 = base64Encode(media.sps.data(), media.sps.size());
        const std::string pps_b64 = base64Encode(media.pps.data(), media.pps.size());
        sdp.addH264Media(control, 0, media.payload_type, clock_rate, sps_b64, pps_b64, media.width, media.height);
    } else {
        const std::string vps_b64 = base64Encode(media.vps.data(), media.vps.size());
        const std::string sps_b64 = base64Encode(media.sps.data(), media.sps.size());
        const std::string pps_b64 = base64Encode(media.pps.data(), media.pps.size());
        sdp.addH265Media(control, 0, media.payload_type, clock_rate, vps_b64, sps_b64, pps_b64, media.width, media.height);
    }

    std::string resp;
    const std::string headers = "Content-Type: application/sdp\r\n";
    if (!impl_->sendRequest("ANNOUNCE", impl_->request_url_, headers, sdp.build(), resp)) return false;
    if (resp.find("200 OK") == std::string::npos) return false;
    impl_->announced_ = true;
    return true;
}

bool RtspPublisher::setup() {
    if (!impl_->connected_ || !impl_->announced_) return false;
    impl_->rtp_sender_ = std::make_unique<RtpSender>();
    if (!impl_->rtp_sender_->init("0.0.0.0", impl_->config_.local_rtp_port)) return false;
    const uint16_t local_rtp = impl_->rtp_sender_->getLocalPort();
    const uint16_t local_rtcp = impl_->rtp_sender_->getLocalRtcpPort();

    std::string resp;
    std::ostringstream headers;
    headers << "Transport: RTP/AVP;unicast;client_port=" << local_rtp << "-" << local_rtcp
            << ";mode=record\r\n";
    std::string track_url = impl_->request_url_ + "/" + (impl_->media_.control_track.empty() ? "streamid=0" : impl_->media_.control_track);
    if (!impl_->sendRequest("SETUP", track_url, headers.str(), "", resp)) return false;
    if (resp.find("200 OK") == std::string::npos) return false;
    if (!impl_->parseSessionAndPorts(resp)) return false;
    if (impl_->server_rtp_port_ == 0) return false;

    impl_->rtp_sender_->setPeer(impl_->host_, impl_->server_rtp_port_,
                                impl_->server_rtcp_port_ == 0 ? static_cast<uint16_t>(impl_->server_rtp_port_ + 1) : impl_->server_rtcp_port_);
    if (impl_->media_.codec == CodecType::H264) {
        impl_->rtp_packer_ = std::make_unique<H264RtpPacker>();
    } else {
        impl_->rtp_packer_ = std::make_unique<H265RtpPacker>();
    }
    impl_->rtp_packer_->setPayloadType(impl_->media_.payload_type);
    // 每个 publisher 实例生成一个随机 SSRC，并联动到 rtp_sender 供 RTCP SR 使用
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist(0x10000000u, 0x7FFFFFFFu);
        const uint32_t ssrc = dist(gen);
        impl_->rtp_packer_->setSsrc(ssrc);
        impl_->rtp_sender_->setSsrc(ssrc);
    }
    impl_->setup_done_ = true;
    return true;
}

bool RtspPublisher::record() {
    if (!impl_->connected_ || !impl_->setup_done_) return false;
    std::string resp;
    if (!impl_->sendRequest("RECORD", impl_->request_url_, "", "", resp)) return false;
    if (resp.find("200 OK") == std::string::npos) return false;
    impl_->recording_ = true;
    return true;
}

bool RtspPublisher::pushFrame(const VideoFrame& frame) {
    if (!impl_->recording_ || !impl_->rtp_packer_ || !impl_->rtp_sender_) return false;
    auto packets = impl_->rtp_packer_->packFrame(frame);
    for (auto& p : packets) {
        impl_->rtp_sender_->sendRtpPacket(p);
        delete[] p.data;
    }
    return true;
}

bool RtspPublisher::pushH264Data(const uint8_t* data, size_t size, uint64_t pts, bool is_key) {
    VideoFrame frame{};
    frame.codec = CodecType::H264;
    frame.type = is_key ? FrameType::IDR : FrameType::P;
    frame.managed_data = makeManagedBuffer(data, size);
    frame.data = frame.managed_data->empty() ? nullptr : frame.managed_data->data();
    frame.size = frame.managed_data->size();
    frame.pts = pts;
    frame.dts = pts;
    frame.width = impl_->media_.width;
    frame.height = impl_->media_.height;
    frame.fps = impl_->media_.fps;
    return pushFrame(frame);
}

bool RtspPublisher::pushH265Data(const uint8_t* data, size_t size, uint64_t pts, bool is_key) {
    VideoFrame frame{};
    frame.codec = CodecType::H265;
    frame.type = is_key ? FrameType::IDR : FrameType::P;
    frame.managed_data = makeManagedBuffer(data, size);
    frame.data = frame.managed_data->empty() ? nullptr : frame.managed_data->data();
    frame.size = frame.managed_data->size();
    frame.pts = pts;
    frame.dts = pts;
    frame.width = impl_->media_.width;
    frame.height = impl_->media_.height;
    frame.fps = impl_->media_.fps;
    return pushFrame(frame);
}

bool RtspPublisher::teardown() {
    if (!impl_->connected_) return false;
    std::string resp;
    // 默认 5s 超时；closeWithTimeout 会用更短超时调用 teardownWithTimeout
    impl_->sendRequest("TEARDOWN", impl_->request_url_, "", "", resp, 5000);
    impl_->recording_ = false;
    impl_->setup_done_ = false;
    impl_->announced_ = false;
    impl_->session_id_.clear();
    impl_->rtp_packer_.reset();
    impl_->rtp_sender_.reset();
    return true;
}

bool RtspPublisher::closeWithTimeout(uint32_t timeout_ms) {
    // 真正遵守超时：用 timeout_ms 作为 sendRequest 的 recv 预算
    if (impl_->connected_) {
        std::string resp;
        const int rtimeout = timeout_ms == 0 ? 1 : static_cast<int>(std::min<uint32_t>(timeout_ms, 5000));
        impl_->sendRequest("TEARDOWN", impl_->request_url_, "", "", resp, rtimeout);
        impl_->recording_ = false;
        impl_->setup_done_ = false;
        impl_->announced_ = false;
        impl_->session_id_.clear();
        impl_->rtp_packer_.reset();
        impl_->rtp_sender_.reset();
    }
    if (impl_->control_socket_) {
        impl_->control_socket_->shutdownReadWrite();
        impl_->control_socket_->close();
    }
    impl_->connected_ = false;
    return true;
}

void RtspPublisher::close() {
    closeWithTimeout(3000);
}

bool RtspPublisher::isConnected() const {
    return impl_->connected_;
}

bool RtspPublisher::isRecording() const {
    return impl_->recording_;
}

} // namespace rtsp
