#include <rtsp-onvif/onvif_daemon.h>

#include "soap_endpoint.h"
#include "ws_discovery.h"
#include "wsse_auth.h"

#if defined(_WIN32) && !defined(NOMINMAX)
    #define NOMINMAX
#endif

#include <rtsp-common/common.h>
#include <rtsp-common/socket.h>   // 复用 Socket 的地址工具

#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "iphlpapi.lib")
    #ifdef min
        #undef min
    #endif
    #ifdef max
        #undef max
    #endif
#else
    #include <sys/types.h>
    #include <ifaddrs.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

namespace rtsp {

namespace {

// 猜一个非 loopback 的 IPv4 本机地址，作为默认 announce_host
std::string guessLocalIp() {
#ifdef _WIN32
    ULONG out_len = 15000;
    std::vector<uint8_t> buf(out_len);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &out_len) == NO_ERROR) {
        auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        for (; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            for (auto* a = adapter->FirstUnicastAddress; a; a = a->Next) {
                if (a->Address.lpSockaddr && a->Address.lpSockaddr->sa_family == AF_INET) {
                    auto* sa = reinterpret_cast<sockaddr_in*>(a->Address.lpSockaddr);
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                    if (std::string(ip) != "127.0.0.1") return ip;
                }
            }
        }
    }
    return "127.0.0.1";
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return "127.0.0.1";
    std::string best = "127.0.0.1";
    for (auto* it = ifs; it != nullptr; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        auto* sa = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        std::string s(ip);
        if (s != "127.0.0.1" && s.rfind("169.254", 0) != 0) {
            best = s;
            break;
        }
    }
    freeifaddrs(ifs);
    return best;
#endif
}

// urn:uuid:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
std::string generateDeviceUuid() {
    std::random_device rd;
    std::mt19937_64 rng;
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd()};
    rng.seed(seq);
    auto part = [&](int nibbles) {
        std::ostringstream oss;
        uint64_t v = rng();
        oss << std::hex << std::setw(nibbles) << std::setfill('0')
            << (v & ((uint64_t{1} << (nibbles * 4)) - 1));
        return oss.str();
    };
    std::ostringstream oss;
    oss << "urn:uuid:" << part(8) << "-" << part(4) << "-4" << part(3)
        << "-a" << part(3) << "-" << part(12);
    return oss.str();
}

std::string buildScopes(const OnvifDeviceInfo& info) {
    // ONVIF 规定的几类 scope
    std::ostringstream oss;
    oss << "onvif://www.onvif.org/type/NetworkVideoTransmitter "
        << "onvif://www.onvif.org/Profile/Streaming "
        << "onvif://www.onvif.org/Profile/S "
        << "onvif://www.onvif.org/name/" << info.model << " "
        << "onvif://www.onvif.org/hardware/" << info.hardware_id << " "
        << "onvif://www.onvif.org/location/country/any";
    return oss.str();
}

}  // namespace

class OnvifDaemon::Impl {
public:
    RtspServer* rtsp_server_ = nullptr;
    OnvifDaemonConfig config_;

    WsseAuthenticator auth_;
    SoapEndpoint      soap_;
    WsDiscoveryResponder discovery_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> probes_{0};
    std::atomic<uint64_t> matches_{0};

    std::string effective_host_;
    std::string effective_rtsp_host_;
    std::string device_uuid_;
};

OnvifDaemon::OnvifDaemon() : impl_(std::make_unique<Impl>()) {}
OnvifDaemon::~OnvifDaemon() { stop(); }

void OnvifDaemon::attachServer(RtspServer* server) {
    impl_->rtsp_server_ = server;
}

void OnvifDaemon::setConfig(const OnvifDaemonConfig& config) {
    impl_->config_ = config;
}

const OnvifDaemonConfig& OnvifDaemon::getConfig() const {
    return impl_->config_;
}

bool OnvifDaemon::start() {
    if (impl_->running_.load()) return false;
    if (!impl_->rtsp_server_) {
        RTSP_LOG_ERROR("OnvifDaemon: attachServer() must be called before start()");
        return false;
    }

    // 确定对外宣告的 host
    impl_->effective_host_ = impl_->config_.announce_host.empty()
        ? guessLocalIp() : impl_->config_.announce_host;
    impl_->effective_rtsp_host_ = impl_->config_.announce_rtsp_host.empty()
        ? impl_->effective_host_ : impl_->config_.announce_rtsp_host;
    impl_->device_uuid_ = generateDeviceUuid();

    // 配置 SOAP endpoint
    impl_->auth_.setCredentials(impl_->config_.auth_username, impl_->config_.auth_password);
    impl_->soap_.setDeviceInfo(impl_->config_.device_info);
    impl_->soap_.setHttpHost(impl_->effective_host_, impl_->config_.http_port);
    impl_->soap_.setRtspHost(impl_->effective_rtsp_host_, impl_->config_.rtsp_port);
    impl_->soap_.setDevicePath(impl_->config_.device_service_path);
    impl_->soap_.setMediaPath(impl_->config_.media_service_path);
    impl_->soap_.setDeviceEndpointUuid(impl_->device_uuid_);
    impl_->soap_.setAuthenticator(&impl_->auth_);
    impl_->soap_.setAnonymousActions(impl_->config_.anonymous_actions);
    impl_->soap_.refreshProfilesFromServer(impl_->rtsp_server_);

    if (!impl_->soap_.start(impl_->config_.http_port)) {
        RTSP_LOG_ERROR("OnvifDaemon: SOAP endpoint failed to start on port " +
                       std::to_string(impl_->config_.http_port));
        return false;
    }

    // 配置 WS-Discovery
    if (impl_->config_.enable_ws_discovery) {
        WsDiscoveryResponder::Config dcfg;
        dcfg.endpoint_uuid = impl_->device_uuid_;
        std::ostringstream xaddr;
        xaddr << "http://" << impl_->effective_host_ << ":" << impl_->config_.http_port
              << impl_->config_.device_service_path;
        dcfg.xaddr = xaddr.str();
        dcfg.scopes = buildScopes(impl_->config_.device_info);
        impl_->discovery_.setConfig(dcfg);
        impl_->discovery_.setOnProbeReceived([this] { impl_->probes_.fetch_add(1); });
        impl_->discovery_.setOnMatchSent([this] { impl_->matches_.fetch_add(1); });
        if (!impl_->discovery_.start()) {
            RTSP_LOG_WARNING("OnvifDaemon: WS-Discovery failed to start (firewall?); "
                             "SOAP endpoint still running");
        }
    }

    impl_->running_.store(true);
    RTSP_LOG_INFO("OnvifDaemon started: device XAddr=http://" + impl_->effective_host_ + ":" +
                  std::to_string(impl_->config_.http_port) + impl_->config_.device_service_path);
    return true;
}

void OnvifDaemon::stop() {
    if (!impl_->running_.exchange(false)) {
        // 仍可能只启了部分组件，保险都停一次
        impl_->discovery_.stop();
        impl_->soap_.stop();
        return;
    }
    impl_->discovery_.stop();
    impl_->soap_.stop();
    RTSP_LOG_INFO("OnvifDaemon stopped");
}

bool OnvifDaemon::isRunning() const {
    return impl_->running_.load();
}

OnvifDaemon::Stats OnvifDaemon::getStats() const {
    Stats s;
    s.discovery_probes_received = impl_->probes_.load();
    s.discovery_matches_sent    = impl_->matches_.load();
    const auto ss = impl_->soap_.getStats();
    s.soap_requests_total       = ss.requests_total;
    s.soap_auth_failures        = ss.auth_failures;
    s.soap_unknown_actions      = ss.unknown_actions;
    return s;
}

}  // namespace rtsp
