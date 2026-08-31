#include <rtsp-publisher/rtsp_publisher.h>
#include <rtsp-common/socket.h>
#include <rtsp-common/sdp.h>
#include <rtsp-common/rtp_packer.h>
#include <rtsp-common/common.h>

#include <chrono>
#include <regex>
#include <sstream>
#include <random>
#include <iomanip>
#include <cctype>
#include <unordered_map>

namespace rtsp {

namespace {

std::shared_ptr<std::vector<uint8_t>> makeManagedBuffer(const uint8_t* data, size_t size) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    if (data && size > 0) {
        buf->assign(data, data + size);
    }
    return buf;
}

// 解析 RTSP 状态行 "RTSP/1.0 200 OK" → 200；非法/解析失败返回 0。
// 取代此前用 resp.find("200 OK") 的子串判断——"200 OK" 可能出现在某个
// 非 200 响应的 header 值或 body 里，造成误判（"连上了没视频"）。
int parseStatusCode(const std::string& response) {
    if (response.compare(0, 5, "RTSP/") != 0) return 0;
    const size_t sp = response.find(' ');
    if (sp == std::string::npos) return 0;
    const size_t start = sp + 1;
    const size_t end = response.find_first_of(" \r\n", start);
    const std::string code = response.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    int out = 0;
    return parseInt32Safe(code, out) ? out : 0;
}

// 解析 Digest 挑战参数：realm="x", nonce="y", qop="auth" 等
std::unordered_map<std::string, std::string> parseAuthParams(const std::string& value) {
    std::unordered_map<std::string, std::string> kv;
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == ',')) pos++;
        const size_t eq = value.find('=', pos);
        if (eq == std::string::npos) break;
        const std::string key = value.substr(pos, eq - pos);
        pos = eq + 1;
        std::string v;
        if (pos < value.size() && value[pos] == '"') {
            const size_t endq = value.find('"', pos + 1);
            if (endq == std::string::npos) break;
            v = value.substr(pos + 1, endq - pos - 1);
            pos = endq + 1;
        } else {
            const size_t comma = value.find(',', pos);
            if (comma == std::string::npos) { v = value.substr(pos); pos = value.size(); }
            else { v = value.substr(pos, comma - pos); pos = comma + 1; }
        }
        kv[key] = v;
    }
    return kv;
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
    std::chrono::steady_clock::time_point last_keepalive_{};

    // RTP 走 UDP，服务器消失后 sendTo 依旧“成功”——断链只能靠 RTSP control TCP 感知：
    // 对端 FIN → recv 返回 0；对端 RST（进程被 kill）→ 后续 send 失败。
    // 这里非阻塞清掉积压的 keepalive 响应，再发一条 GET_PARAMETER（不等回包）。
    bool checkControlAlive() {
        if (!control_socket_) return false;
        uint8_t buf[2048];
        for (;;) {
            const ssize_t n = control_socket_->recv(buf, sizeof(buf), 0);
            if (n == 0) return false;   // 对端已关闭
            if (n < 0) break;           // 暂无数据（EAGAIN）
        }
        std::ostringstream req;
        req << "GET_PARAMETER " << request_url_ << " RTSP/1.0\r\nCSeq: " << ++cseq_ << "\r\n";
        if (!session_id_.empty()) req << "Session: " << session_id_ << "\r\n";
        req << "User-Agent: " << config_.user_agent << "\r\n\r\n";
        const std::string wire = req.str();
        return control_socket_->sendAll(reinterpret_cast<const uint8_t*>(wire.data()),
                                        wire.size(), 1000) == static_cast<ssize_t>(wire.size());
    }

    // 断链后把状态打回未连接，让上层能走完整的 open→announce→setup→record 重连
    void markDisconnected() {
        recording_ = false;
        setup_done_ = false;
        announced_ = false;
        session_id_.clear();
        rtp_packer_.reset();
        rtp_sender_.reset();
        if (control_socket_) {
            control_socket_->shutdownReadWrite();
            control_socket_->close();
            control_socket_.reset();
        }
        connected_ = false;
    }

    // Digest 鉴权状态
    std::string auth_user_;
    std::string auth_pass_;
    std::string digest_realm_;
    std::string digest_nonce_;
    std::string digest_qop_;
    uint32_t digest_nc_ = 0;
    bool use_digest_ = false;

    bool parseUrl(const std::string& url) {
        if (url.find("rtsp://") != 0) return false;
        std::string no_scheme = url.substr(7);
        size_t slash = no_scheme.find('/');
        std::string host_port = (slash == std::string::npos) ? no_scheme : no_scheme.substr(0, slash);
        path_ = (slash == std::string::npos) ? "/" : no_scheme.substr(slash);
        if (path_.empty()) path_ = "/";

        // 提取 user:pass@ 凭据（若 URL 里带），并从 authority 中剥离，
        // 这样 request_url_ 不含凭据、host:port 解析也不受 userinfo 的 ':' 干扰
        const size_t at = host_port.find('@');
        if (at != std::string::npos) {
            const std::string userinfo = host_port.substr(0, at);
            host_port = host_port.substr(at + 1);
            const size_t uc = userinfo.find(':');
            if (uc != std::string::npos) {
                auth_user_ = userinfo.substr(0, uc);
                auth_pass_ = userinfo.substr(uc + 1);
            } else {
                auth_user_ = userinfo;
            }
        }

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
        if (use_digest_ && !auth_user_.empty()) {
            const std::string auth = buildDigestAuthorization(method, uri);
            if (!auth.empty()) req << "Authorization: " << auth << "\r\n";
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

    bool parseWwwAuthenticate(const std::string& response) {
        static const std::regex ww_re("WWW-Authenticate:\\s*([^\\r\\n]+)", std::regex::icase);
        std::smatch m;
        if (!std::regex_search(response, m, ww_re)) return false;
        const std::string challenge = m[1].str();
        if (challenge.rfind("Digest ", 0) != 0) return false;   // 仅支持 Digest
        auto p = parseAuthParams(challenge.substr(7));
        digest_realm_ = p["realm"];
        digest_nonce_ = p["nonce"];
        digest_qop_.clear();
        if (!p["qop"].empty()) {
            // qop 可能是 "auth,auth-int" 列表：优先 auth，否则取第一个
            const std::string qop_list = p["qop"];
            std::vector<std::string> tokens;
            size_t pos = 0;
            while (pos < qop_list.size()) {
                const auto comma = qop_list.find(',', pos);
                const auto end = (comma == std::string::npos) ? qop_list.size() : comma;
                std::string tok = qop_list.substr(pos, end - pos);
                while (!tok.empty() && std::isspace((unsigned char)tok.front())) tok.erase(tok.begin());
                while (!tok.empty() && std::isspace((unsigned char)tok.back()))  tok.pop_back();
                if (!tok.empty()) tokens.push_back(tok);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            for (const auto& t : tokens) if (t == "auth") { digest_qop_ = "auth"; break; }
            if (digest_qop_.empty() && !tokens.empty()) digest_qop_ = tokens.front();
        }
        use_digest_ = !digest_realm_.empty() && !digest_nonce_.empty();
        return use_digest_;
    }

    std::string buildDigestAuthorization(const std::string& method, const std::string& uri) {
        if (auth_user_.empty() || auth_pass_.empty() || digest_nonce_.empty() || digest_realm_.empty()) {
            return "";
        }
        const std::string ha1 = md5Hex(auth_user_ + ":" + digest_realm_ + ":" + auth_pass_);
        const std::string ha2 = md5Hex(method + ":" + uri);
        std::string response, nc, cnonce;
        if (digest_qop_.empty()) {
            // RFC 2069: response = MD5(HA1:nonce:HA2)
            response = md5Hex(ha1 + ":" + digest_nonce_ + ":" + ha2);
        } else {
            // RFC 2617: response = MD5(HA1:nonce:nc:cnonce:qop:HA2)
            digest_nc_++;
            std::ostringstream nc_ss;
            nc_ss << std::hex << std::setw(8) << std::setfill('0') << digest_nc_;
            nc = nc_ss.str();
            cnonce = md5Hex(std::to_string(digest_nc_) + ":" + auth_user_ + ":" + uri).substr(0, 16);
            response = md5Hex(ha1 + ":" + digest_nonce_ + ":" + nc + ":" + cnonce + ":" + digest_qop_ + ":" + ha2);
        }
        std::ostringstream out;
        out << "Digest username=\"" << auth_user_ << "\", realm=\"" << digest_realm_
            << "\", nonce=\"" << digest_nonce_ << "\", uri=\"" << uri
            << "\", response=\"" << response << "\"";
        if (!digest_qop_.empty()) {
            out << ", qop=" << digest_qop_ << ", nc=" << nc << ", cnonce=\"" << cnonce << "\"";
        }
        return out.str();
    }

    // 发请求；若 401 且配置了凭据，解析 Digest 挑战并带 Authorization 重发一次
    // （也覆盖 nonce 过期、服务端重新挑战的情形）。out_status = 最终状态码。
    bool sendRequestWithAuth(const std::string& method, const std::string& uri,
                             const std::string& headers, const std::string& body,
                             std::string& response, int& out_status, int recv_timeout_ms = 5000) {
        if (!sendRequest(method, uri, headers, body, response, recv_timeout_ms)) return false;
        out_status = parseStatusCode(response);
        if (out_status == 401 && !auth_user_.empty() && parseWwwAuthenticate(response)) {
            if (!sendRequest(method, uri, headers, body, response, recv_timeout_ms)) return false;
            out_status = parseStatusCode(response);
        }
        return true;
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
    // URL 未带凭据时回退到 config（URL 优先）
    if (impl_->auth_user_.empty()) {
        impl_->auth_user_ = impl_->config_.username;
        impl_->auth_pass_ = impl_->config_.password;
    }
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
    int status = 0;
    if (!impl_->sendRequestWithAuth("ANNOUNCE", impl_->request_url_, headers, sdp.build(), resp, status)) return false;
    if (status != 200) return false;
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
    int status = 0;
    if (!impl_->sendRequestWithAuth("SETUP", track_url, headers.str(), "", resp, status)) return false;
    if (status != 200) return false;
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
    int status = 0;
    if (!impl_->sendRequestWithAuth("RECORD", impl_->request_url_, "", "", resp, status)) return false;
    if (status != 200) return false;
    impl_->recording_ = true;
    impl_->last_keepalive_ = std::chrono::steady_clock::now();
    return true;
}

bool RtspPublisher::pushFrame(const VideoFrame& frame) {
    if (!impl_->recording_ || !impl_->rtp_packer_ || !impl_->rtp_sender_) return false;

    // 周期探活控制连接：这是感知“推流服务器被重启/关闭”的唯一手段（见 checkControlAlive）
    const auto now = std::chrono::steady_clock::now();
    if (now - impl_->last_keepalive_ >= std::chrono::seconds(5)) {
        impl_->last_keepalive_ = now;
        if (!impl_->checkControlAlive()) {
            impl_->markDisconnected();
            return false;
        }
    }

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
