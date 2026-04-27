#include "ws_discovery.h"

#if defined(_WIN32) && !defined(NOMINMAX)
    #define NOMINMAX
#endif

#include <rtsp-common/common.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <BaseTsd.h>
    #pragma comment(lib, "ws2_32.lib")
    #ifdef min
        #undef min
    #endif
    #ifdef max
        #undef max
    #endif
    // MSVC 无 ssize_t；Windows SDK 的 SSIZE_T (BaseTsd.h) 即 signed intptr_t
    typedef SSIZE_T ssize_t;
    typedef int socklen_t;
    using pollfd_t = WSAPOLLFD;
    static int ws_poll(pollfd_t* fds, size_t nfds, int timeout_ms) {
        return WSAPoll(fds, static_cast<ULONG>(nfds), timeout_ms);
    }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    using pollfd_t = pollfd;
    static int ws_poll(pollfd_t* fds, size_t nfds, int timeout_ms) {
        return ::poll(fds, nfds, timeout_ms);
    }
#endif

namespace rtsp {

namespace {

constexpr const char* kMulticastAddr = "239.255.255.250";
constexpr uint16_t kMulticastPort = 3702;

// WS-Discovery / ONVIF 需要的 XML 模板。用纯字符串拼装不引 XML 库。
// ProbeMatch 的结构参考 ONVIF Network Interface Specification v21.12 §7.3
//
// 必填字段：
//   - RelatesTo = 请求的 MessageID
//   - MessageID = 本次响应的 UUID
//   - Types = dn:NetworkVideoTransmitter
//   - Scopes = 设备信息（厂商/型号/hardware 等，urn 形式）
//   - XAddrs = SOAP endpoint URL（HTTP）
//   - MetadataVersion = 1
std::string buildProbeMatch(const std::string& relates_to_uuid,
                            const std::string& device_endpoint_uuid,
                            const std::string& xaddr,
                            const std::string& scopes) {
    // 每次响应用新的 MessageID
    // 这里用简单 counter+随机保证唯一（WS-Discovery 不要求强密码学随机）
    static std::atomic<uint64_t> counter{1};
    std::ostringstream msgid;
    msgid << "urn:uuid:00000000-0000-0000-0000-"
          << std::hex << counter.fetch_add(1) << std::dec;

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<soap:Envelope "
        <<   "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
        <<   "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        <<   "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        <<   "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
        <<   "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        << "<soap:Header>"
        <<   "<wsa:MessageID>" << msgid.str() << "</wsa:MessageID>"
        <<   "<wsa:RelatesTo>" << relates_to_uuid << "</wsa:RelatesTo>"
        <<   "<wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>"
        <<   "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>"
        << "</soap:Header>"
        << "<soap:Body>"
        <<   "<wsd:ProbeMatches>"
        <<     "<wsd:ProbeMatch>"
        <<       "<wsa:EndpointReference>"
        <<         "<wsa:Address>" << device_endpoint_uuid << "</wsa:Address>"
        <<       "</wsa:EndpointReference>"
        <<       "<wsd:Types>dn:NetworkVideoTransmitter tds:Device</wsd:Types>"
        <<       "<wsd:Scopes>" << scopes << "</wsd:Scopes>"
        <<       "<wsd:XAddrs>" << xaddr << "</wsd:XAddrs>"
        <<       "<wsd:MetadataVersion>1</wsd:MetadataVersion>"
        <<     "</wsd:ProbeMatch>"
        <<   "</wsd:ProbeMatches>"
        << "</soap:Body>"
        << "</soap:Envelope>";
    return oss.str();
}

// 从 Probe 请求 XML 里提取 wsa:MessageID（作为 RelatesTo 的 UUID）。
// Probe 也可能带 Types 过滤（如只找 NetworkVideoTransmitter），这里宽松处理：
// 只要是 ONVIF 或不写 Types 的 Probe 都回。
bool parseProbeMessageId(const std::string& xml, std::string* out_msgid, bool* out_onvif) {
    // 手写轻量解析，避免拉 XML 库。MessageID 模式：<...MessageID>XXX</...MessageID>
    // 不同命名空间前缀（wsa:/w:）都允许。
    static const std::regex msgid_re(R"(<(?:[^>]*:)?MessageID[^>]*>([^<]+)</(?:[^>]*:)?MessageID>)",
                                     std::regex::icase);
    std::smatch m;
    if (!std::regex_search(xml, m, msgid_re)) return false;
    *out_msgid = m[1].str();

    // 判断是否 ONVIF / NetworkVideoTransmitter 类型
    // Probe 的 Types 字段如 <d:Types>dn:NetworkVideoTransmitter</d:Types>
    // 若没有 Types 节点（泛扫），也认为是想发现任何设备 → 视为 ONVIF 允许回
    const bool has_nvt = xml.find("NetworkVideoTransmitter") != std::string::npos ||
                         xml.find("onvif") != std::string::npos;
    const bool has_types_tag = xml.find("Types>") != std::string::npos;
    *out_onvif = has_nvt || !has_types_tag;
    return true;
}

int createSocket() {
#ifdef _WIN32
    return static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
#else
    return ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
}

void closeSocketFd(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

}  // namespace

class WsDiscoveryResponder::Impl {
public:
    Config config_;
    ProbeReceivedCallback on_probe_;
    ProbeReceivedCallback on_match_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<int> sock_fd_{-1};

    bool bindAndJoin() {
        int fd = createSocket();
        if (fd < 0) return false;

        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kMulticastPort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            closeSocketFd(fd);
            return false;
        }

        ip_mreq mreq{};
        // inet_addr 在 Windows SDK 上会报 deprecation；改用 inet_pton（跨平台）
        if (::inet_pton(AF_INET, kMulticastAddr, &mreq.imr_multiaddr) != 1) {
            closeSocketFd(fd);
            return false;
        }
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         reinterpret_cast<const char*>(&mreq), sizeof(mreq)) < 0) {
            RTSP_LOG_WARNING("WsDiscovery: failed to join multicast group (firewall?)");
            closeSocketFd(fd);
            return false;
        }

        // 关闭多播回环（不把自己发的包收回来）
        unsigned char loop = 0;
        ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                     reinterpret_cast<const char*>(&loop), sizeof(loop));

        sock_fd_.store(fd);
        return true;
    }

    void receiveLoop() {
        while (running_.load()) {
            const int fd = sock_fd_.load();
            if (fd < 0) break;

            pollfd_t pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            const int pr = ws_poll(&pfd, 1, 300);
            if (!running_.load()) break;
            if (pr <= 0) continue;

            char buf[8192];
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0,
                                         reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) continue;
            buf[n] = '\0';

            const std::string xml(buf, static_cast<size_t>(n));
            // 仅处理 Probe，对 Hello/Bye/ProbeMatches 一律忽略
            if (xml.find("Probe") == std::string::npos ||
                xml.find("ProbeMatches") != std::string::npos) {
                continue;
            }

            std::string msgid;
            bool is_onvif = false;
            if (!parseProbeMessageId(xml, &msgid, &is_onvif)) continue;
            if (!is_onvif) continue;

            if (on_probe_) on_probe_();

            const std::string resp = buildProbeMatch(msgid,
                                                     config_.endpoint_uuid,
                                                     config_.xaddr,
                                                     config_.scopes);
            // 单播回发给 Probe 发起方
            const ssize_t sent = ::sendto(fd, resp.data(),
#ifdef _WIN32
                                          static_cast<int>(resp.size()),
#else
                                          resp.size(),
#endif
                                          0,
                                          reinterpret_cast<sockaddr*>(&from), from_len);
            if (sent > 0 && on_match_) on_match_();
        }

        const int fd = sock_fd_.exchange(-1);
        if (fd >= 0) closeSocketFd(fd);
    }
};

WsDiscoveryResponder::WsDiscoveryResponder() : impl_(std::make_unique<Impl>()) {}
WsDiscoveryResponder::~WsDiscoveryResponder() { stop(); }

void WsDiscoveryResponder::setConfig(const Config& cfg) { impl_->config_ = cfg; }
void WsDiscoveryResponder::setOnProbeReceived(ProbeReceivedCallback cb) { impl_->on_probe_ = std::move(cb); }
void WsDiscoveryResponder::setOnMatchSent(ProbeReceivedCallback cb) { impl_->on_match_ = std::move(cb); }

bool WsDiscoveryResponder::start() {
    if (impl_->running_.load()) return false;
    if (!impl_->bindAndJoin()) return false;
    impl_->running_.store(true);
    impl_->thread_ = std::thread([this] { impl_->receiveLoop(); });
    RTSP_LOG_INFO("WsDiscovery started (multicast " + std::string(kMulticastAddr) + ":3702)");
    return true;
}

void WsDiscoveryResponder::stop() {
    if (!impl_->running_.exchange(false)) return;
    // 仅 shutdown 唤醒 worker 的 recvfrom，关 fd 交给 worker 自己做。
    // 这样避免主线程 close(fd) 与 worker 的 recvfrom(fd) 并发访问同一 fd 号（TSan race）。
    const int fd = impl_->sock_fd_.load();
    if (fd >= 0) {
#ifdef _WIN32
        ::shutdown(fd, SD_BOTH);
#else
        ::shutdown(fd, SHUT_RDWR);
#endif
    }
    if (impl_->thread_.joinable()) impl_->thread_.join();
    RTSP_LOG_INFO("WsDiscovery stopped");
}

bool WsDiscoveryResponder::isRunning() const {
    return impl_->running_.load();
}

}  // namespace rtsp
