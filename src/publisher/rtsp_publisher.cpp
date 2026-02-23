#include <rtsp-publisher/rtsp_publisher.h>
#include <rtsp-common/socket.h>
#include <rtsp-common/sdp.h>
#include <rtsp-common/rtp_packer.h>
#include <rtsp-common/common.h>

#include <regex>
#include <sstream>

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
            port_ = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
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
                     std::string& response) {
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
        if (control_socket_->send(reinterpret_cast<const uint8_t*>(wire.data()), wire.size()) <= 0) {
            return false;
        }

        char buf[8192];
        ssize_t n = control_socket_->recv(reinterpret_cast<uint8_t*>(buf), sizeof(buf), 5000);
        if (n <= 0) return false;
        response.assign(buf, static_cast<size_t>(n));
        return true;
    }

    bool parseSessionAndPorts(const std::string& response) {
        std::smatch m;
        std::regex session_regex("Session:\\s*([^;\\r\\n]+)", std::regex::icase);
        if (std::regex_search(response, m, session_regex)) {
            session_id_ = m[1].str();
        }
        std::regex server_port_regex("server_port=(\\d+)-(\\d+)", std::regex::icase);
        if (std::regex_search(response, m, server_port_regex)) {
            server_rtp_port_ = static_cast<uint16_t>(std::stoi(m[1].str()));
            server_rtcp_port_ = static_cast<uint16_t>(std::stoi(m[2].str()));
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
    sdp.setConnection("IN", "IP4", "0.0.0.0");
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
    headers << "Transport: RTP/AVP;unicast;client_port=" << local_rtp << "-" << local_rtcp << "\r\n";
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
    impl_->sendRequest("TEARDOWN", impl_->request_url_, "", "", resp);
    impl_->recording_ = false;
    impl_->setup_done_ = false;
    impl_->announced_ = false;
    impl_->session_id_.clear();
    impl_->rtp_packer_.reset();
    impl_->rtp_sender_.reset();
    return true;
}

bool RtspPublisher::closeWithTimeout(uint32_t timeout_ms) {
    (void)timeout_ms;
    teardown();
    if (impl_->control_socket_) {
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
