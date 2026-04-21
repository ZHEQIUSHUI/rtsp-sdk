#pragma once

// ONVIF Profile S（裁剪版）支持：WS-Discovery（UDP 239.255.255.250:3702）+
// SOAP over HTTP（Device + Media 服务）。附带可选 WS-Security UsernameToken
// PasswordDigest 鉴权。本模块纯附加，不侵入 RtspServer；用户显式 attach 才生效。
//
// 目标使用场景：工业局域网里 NVR / VMS / ONVIF Device Manager 自动发现设备
// 并取到 RTSP URL 直接播放。实现聚焦 Profile S 的最小必备 operation，
// 不做 PTZ/事件/录像/imaging 等扩展子协议。

#include <rtsp-common/common.h>
#include <rtsp-server/rtsp_server.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {

struct OnvifDeviceInfo {
    std::string manufacturer = "rtsp-sdk";
    std::string model        = "virtual-camera";
    std::string firmware     = "1.0.0";
    std::string serial       = "000000";
    std::string hardware_id  = "generic";
};

struct OnvifDaemonConfig {
    // SOAP HTTP 监听端口（ONVIF 默认并不强制，习惯 80/8080）
    uint16_t http_port = 8080;
    // SOAP endpoint 路径
    std::string device_service_path = "/onvif/device_service";
    std::string media_service_path  = "/onvif/media_service";
    // 对外宣告的 HTTP host（XAddr 里会填）。空字符串时自动用本机 IP。
    std::string announce_host;
    // 对外宣告的 RTSP host：生成 GetStreamUri 的 rtsp://host:port/path。
    // 空字符串时自动用本机 IP。注意与 RTSP server 的监听端口搭配。
    std::string announce_rtsp_host;
    uint16_t rtsp_port = 554;
    // 是否启用 WS-Discovery UDP 响应器
    bool enable_ws_discovery = true;
    // 设备元信息
    OnvifDeviceInfo device_info;
    // 鉴权（WS-Security UsernameToken，PasswordDigest 模式）。
    // 非空即启用；所有 SOAP 请求（除 GetSystemDateAndTime 等允许匿名的）
    // 都必须带有效 Token。
    std::string auth_username;
    std::string auth_password;
    // 允许匿名的 SOAP action（例如对时），默认只允许 GetSystemDateAndTime
    // 与 GetCapabilities（有些客户端先查能力再鉴权）。
    std::vector<std::string> anonymous_actions = {
        "GetSystemDateAndTime",
        "GetCapabilities"
    };
};

// 生命周期：
//   1. OnvifDaemon d;
//   2. d.attachServer(&server);   // server 必须先 init + addPath
//   3. d.setConfig(cfg);
//   4. d.start();  // 起 HTTP + WS-Discovery 两个线程
//   5. ...        // server 运行期间 daemon 持续响应 ONVIF 请求
//   6. d.stop();
class OnvifDaemon {
public:
    OnvifDaemon();
    ~OnvifDaemon();

    OnvifDaemon(const OnvifDaemon&) = delete;
    OnvifDaemon& operator=(const OnvifDaemon&) = delete;

    // 关联一个 RtspServer 实例，daemon 从中动态枚举 PathConfig 作为 profile
    // 来源。RtspServer 生命周期必须长于 daemon。
    void attachServer(RtspServer* server);

    void setConfig(const OnvifDaemonConfig& config);
    const OnvifDaemonConfig& getConfig() const;

    // 启动：开始监听 HTTP + WS-Discovery。已 start 则返回 false。
    bool start();
    // 停止：shutdown HTTP + 退出 WS-Discovery 线程。幂等。
    void stop();
    bool isRunning() const;

    // 统计（调试/监控用）
    struct Stats {
        uint64_t discovery_probes_received = 0;
        uint64_t discovery_matches_sent    = 0;
        uint64_t soap_requests_total       = 0;
        uint64_t soap_auth_failures        = 0;
        uint64_t soap_unknown_actions      = 0;
    };
    Stats getStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rtsp
