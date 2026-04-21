#pragma once

// ONVIF SOAP endpoint（HTTP 上的 Device + Media 服务），用 cpp-httplib 承载。
//
// 实现范围（Profile S 裁剪）：
//   Device 服务：
//     - GetDeviceInformation
//     - GetCapabilities
//     - GetServices
//     - GetSystemDateAndTime
//     - GetWsdlUrl（返回 404 让客户端知道我们不提供 WSDL 导出）
//   Media 服务：
//     - GetProfiles
//     - GetVideoSources
//     - GetStreamUri
//     - GetSnapshotUri（返回空 URI，表示不支持抓图；合规，客户端能正常降级）
//
// 其他 SOAP operation 一律回 SOAP Fault，客户端看到后会优雅降级。

#include <rtsp-onvif/onvif_daemon.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {

class WsseAuthenticator;

class SoapEndpoint {
public:
    struct MediaProfile {
        std::string name;          // 给客户端看的名字
        std::string token;         // profile token（会被 GetStreamUri 引用）
        std::string rtsp_path;     // 对应 RTSP 路径，如 /live/stream
        uint32_t width  = 1920;
        uint32_t height = 1080;
        uint32_t fps    = 30;
        CodecType codec = CodecType::H264;
    };

    SoapEndpoint();
    ~SoapEndpoint();

    SoapEndpoint(const SoapEndpoint&) = delete;
    SoapEndpoint& operator=(const SoapEndpoint&) = delete;

    // 配置：由 OnvifDaemon 传入
    void setDeviceInfo(const OnvifDeviceInfo& info);
    void setHttpHost(const std::string& host, uint16_t port);
    void setRtspHost(const std::string& host, uint16_t port);
    void setDeviceEndpointUuid(const std::string& urn_uuid);
    void setDevicePath(const std::string& path);
    void setMediaPath(const std::string& path);
    void setAuthenticator(WsseAuthenticator* auth);
    void setAnonymousActions(const std::vector<std::string>& actions);

    // 从 RtspServer 当前 path 列表刷新 profile 集合
    void refreshProfilesFromServer(RtspServer* server);
    void setProfiles(std::vector<MediaProfile> profiles);

    bool start(uint16_t http_port);
    void stop();
    bool isRunning() const;

    struct Stats {
        uint64_t requests_total    = 0;
        uint64_t auth_failures     = 0;
        uint64_t unknown_actions   = 0;
    };
    Stats getStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rtsp
