#include "soap_endpoint.h"
#include "soap_templates.h"
#include "wsse_auth.h"

#if defined(_WIN32) && !defined(NOMINMAX)
    #define NOMINMAX
#endif

// 必须在 <httplib.h> 之前设置一些选项：保持默认即可（不启用 OpenSSL/zlib/brotli）
#include "../../third_party/httplib.h"

#include <rtsp-common/common.h>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace rtsp {

namespace {

// 从 SOAP Body 里识别 operation（tag local name）。
// 返回形如 "GetDeviceInformation" / "GetStreamUri"。
// 兼容 SOAPAction HTTP header 和 Body 内 tag 两种检测方式。
std::string detectAction(const std::string& body, const std::string& soap_action_hdr) {
    // 优先从 SOAPAction header 拿（如 "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation"）
    if (!soap_action_hdr.empty()) {
        std::string h = soap_action_hdr;
        // 去引号
        if (!h.empty() && h.front() == '"') h.erase(0, 1);
        if (!h.empty() && h.back()  == '"') h.pop_back();
        const auto slash = h.rfind('/');
        if (slash != std::string::npos) return h.substr(slash + 1);
    }
    // 退回到 body 里找第一个 action 风格 tag
    // 模式：<(?:[^>]+:)?Get[A-Za-z]+...>
    static const std::regex re(R"(<(?:[^>]+:)?(Get[A-Za-z]+)[^>]*>)");
    std::smatch m;
    if (std::regex_search(body, m, re)) return m[1].str();
    return {};
}

// 从 body 里抽一个参数值（用于 GetStreamUri 的 ProfileToken 等）
std::string extractParam(const std::string& body, const std::string& tag) {
    const std::string pat =
        "<(?:[^>]*:)?" + tag + "[^>]*>([^<]*)</(?:[^>]*:)?" + tag + ">";
    std::regex re(pat, std::regex::icase);
    std::smatch m;
    if (std::regex_search(body, m, re)) return m[1].str();
    return {};
}

}  // namespace

class SoapEndpoint::Impl {
public:
    OnvifDeviceInfo device_info_;
    std::string http_host_;
    uint16_t http_port_ = 8080;
    std::string rtsp_host_;
    uint16_t rtsp_port_ = 554;
    std::string device_endpoint_uuid_;
    std::string device_path_ = "/onvif/device_service";
    std::string media_path_  = "/onvif/media_service";
    WsseAuthenticator* auth_ = nullptr;
    std::vector<std::string> anon_actions_;

    mutable std::mutex profiles_mutex_;
    std::vector<MediaProfile> profiles_;

    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> requests_total_{0};
    std::atomic<uint64_t> auth_failures_{0};
    std::atomic<uint64_t> unknown_actions_{0};

    std::string deviceXAddr() const {
        std::ostringstream oss;
        oss << "http://" << http_host_ << ":" << http_port_ << device_path_;
        return oss.str();
    }
    std::string mediaXAddr() const {
        std::ostringstream oss;
        oss << "http://" << http_host_ << ":" << http_port_ << media_path_;
        return oss.str();
    }

    bool isAnonymous(const std::string& action) const {
        for (const auto& a : anon_actions_) if (a == action) return true;
        return false;
    }

    void sendSoap(httplib::Response& res, const std::string& body, int http_status = 200) {
        res.status = http_status;
        res.set_header("Content-Type", "application/soap+xml; charset=utf-8");
        res.set_content(body, "application/soap+xml");
    }

    // 通用入口：解析 body，鉴权，分发 action
    void handle(const httplib::Request& req, httplib::Response& res, bool is_device_service) {
        requests_total_++;
        const std::string& body = req.body;
        const std::string soap_action = req.has_header("SOAPAction")
            ? req.get_header_value("SOAPAction") : std::string();
        const std::string action = detectAction(body, soap_action);

        if (action.empty()) {
            unknown_actions_++;
            sendSoap(res, soap::faultResponse("Cannot determine SOAP action"), 400);
            return;
        }

        // 鉴权（除白名单 action 外）
        if (auth_ && auth_->enabled() && !isAnonymous(action)) {
            const auto r = auth_->verify(body);
            if (!r.ok) {
                auth_failures_++;
                sendSoap(res, soap::faultResponse("Authentication failed: " + r.failure_reason), 401);
                return;
            }
        }

        // 路由
        if (is_device_service) {
            if (action == "GetDeviceInformation") {
                sendSoap(res, soap::getDeviceInformationResponse(device_info_));
            } else if (action == "GetSystemDateAndTime") {
                sendSoap(res, soap::getSystemDateAndTimeResponse());
            } else if (action == "GetCapabilities") {
                sendSoap(res, soap::getCapabilitiesResponse(mediaXAddr(), deviceXAddr()));
            } else if (action == "GetServices") {
                sendSoap(res, soap::getServicesResponse(mediaXAddr(), deviceXAddr()));
            } else {
                unknown_actions_++;
                sendSoap(res, soap::faultResponse("Unsupported device action: " + action), 400);
            }
        } else {
            std::vector<MediaProfile> profiles_snapshot;
            {
                std::lock_guard<std::mutex> lock(profiles_mutex_);
                profiles_snapshot = profiles_;
            }
            if (action == "GetProfiles") {
                sendSoap(res, soap::getProfilesResponse(profiles_snapshot));
            } else if (action == "GetVideoSources") {
                sendSoap(res, soap::getVideoSourcesResponse(profiles_snapshot));
            } else if (action == "GetStreamUri") {
                // 从 body 中取 ProfileToken；匹配不到就用第一个 profile
                std::string token = extractParam(body, "ProfileToken");
                std::string rtsp_path;
                for (const auto& p : profiles_snapshot) {
                    if (p.token == token) { rtsp_path = p.rtsp_path; break; }
                }
                if (rtsp_path.empty() && !profiles_snapshot.empty()) {
                    rtsp_path = profiles_snapshot.front().rtsp_path;
                }
                if (rtsp_path.empty()) {
                    sendSoap(res, soap::faultResponse("No RTSP profile available"), 400);
                } else {
                    sendSoap(res, soap::getStreamUriResponse(rtsp_host_, rtsp_port_, rtsp_path));
                }
            } else if (action == "GetSnapshotUri") {
                sendSoap(res, soap::getSnapshotUriResponse());
            } else {
                unknown_actions_++;
                sendSoap(res, soap::faultResponse("Unsupported media action: " + action), 400);
            }
        }
    }
};

SoapEndpoint::SoapEndpoint() : impl_(std::make_unique<Impl>()) {}
SoapEndpoint::~SoapEndpoint() { stop(); }

void SoapEndpoint::setDeviceInfo(const OnvifDeviceInfo& info)      { impl_->device_info_ = info; }
void SoapEndpoint::setHttpHost(const std::string& host, uint16_t port) {
    impl_->http_host_ = host;
    impl_->http_port_ = port;
}
void SoapEndpoint::setRtspHost(const std::string& host, uint16_t port) {
    impl_->rtsp_host_ = host;
    impl_->rtsp_port_ = port;
}
void SoapEndpoint::setDeviceEndpointUuid(const std::string& urn) { impl_->device_endpoint_uuid_ = urn; }
void SoapEndpoint::setDevicePath(const std::string& p)           { impl_->device_path_ = p; }
void SoapEndpoint::setMediaPath(const std::string& p)            { impl_->media_path_  = p; }
void SoapEndpoint::setAuthenticator(WsseAuthenticator* a)        { impl_->auth_ = a; }
void SoapEndpoint::setAnonymousActions(const std::vector<std::string>& a) { impl_->anon_actions_ = a; }

void SoapEndpoint::setProfiles(std::vector<MediaProfile> profiles) {
    std::lock_guard<std::mutex> lock(impl_->profiles_mutex_);
    impl_->profiles_ = std::move(profiles);
}

bool SoapEndpoint::start(uint16_t http_port) {
    if (impl_->running_.load()) return false;
    impl_->http_port_ = http_port;

    auto server = std::make_unique<httplib::Server>();
    // Device service
    server->Post(impl_->device_path_, [this](const httplib::Request& req, httplib::Response& res) {
        impl_->handle(req, res, /*is_device_service=*/true);
    });
    // Media service
    server->Post(impl_->media_path_, [this](const httplib::Request& req, httplib::Response& res) {
        impl_->handle(req, res, /*is_device_service=*/false);
    });
    // 一些客户端先 GET 看设备能不能响应，给 200
    server->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ONVIF Device", "text/plain");
    });

    impl_->server_ = std::move(server);
    impl_->running_.store(true);

    // listen 是阻塞的；在后台线程里跑
    impl_->server_thread_ = std::thread([this, http_port] {
        const bool ok = impl_->server_->listen("0.0.0.0", http_port);
        if (!ok) {
            RTSP_LOG_ERROR("ONVIF SOAP: failed to bind http port " + std::to_string(http_port));
        }
        impl_->running_.store(false);
    });

    // 给 httplib 一点时间启动 listen；若启动失败 running_ 会被置 false
    for (int i = 0; i < 50; ++i) {
        if (!impl_->running_.load()) break;
        if (impl_->server_->is_running()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!impl_->running_.load()) {
        if (impl_->server_thread_.joinable()) impl_->server_thread_.join();
        impl_->server_.reset();
        return false;
    }
    return true;
}

void SoapEndpoint::stop() {
    if (!impl_->running_.exchange(false)) {
        // 即便 flag 已经是 false，也要确保 listen 线程收尾
        if (impl_->server_) impl_->server_->stop();
        if (impl_->server_thread_.joinable()) impl_->server_thread_.join();
        impl_->server_.reset();
        return;
    }
    if (impl_->server_) impl_->server_->stop();
    if (impl_->server_thread_.joinable()) impl_->server_thread_.join();
    impl_->server_.reset();
}

bool SoapEndpoint::isRunning() const {
    return impl_->running_.load() && impl_->server_ && impl_->server_->is_running();
}

SoapEndpoint::Stats SoapEndpoint::getStats() const {
    return {
        impl_->requests_total_.load(),
        impl_->auth_failures_.load(),
        impl_->unknown_actions_.load(),
    };
}

void SoapEndpoint::refreshProfilesFromServer(RtspServer* server) {
    if (!server) return;
    std::vector<MediaProfile> profiles;
    const auto paths = server->getPathsSnapshot();
    profiles.reserve(paths.size());
    int idx = 0;
    for (const auto& p : paths) {
        MediaProfile mp;
        // 生成一个稳定可读的 token，基于路径（去掉 '/' 与奇怪字符）
        std::string token;
        for (char c : p.path) {
            if (c == '/' || c == ' ') continue;
            token.push_back(c);
        }
        if (token.empty()) token = "profile" + std::to_string(idx);
        mp.token = token;
        mp.name  = "Profile_" + std::to_string(idx);
        mp.rtsp_path = p.path;
        mp.width  = p.width;
        mp.height = p.height;
        mp.fps    = p.fps;
        mp.codec  = p.codec;
        profiles.push_back(std::move(mp));
        ++idx;
    }
    setProfiles(std::move(profiles));
}

}  // namespace rtsp
