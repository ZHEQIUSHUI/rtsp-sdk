#pragma once

// WS-Discovery Probe 响应器（ONVIF 发现部分）。
// 标准：UDP 多播 239.255.255.250:3702，收到 <Probe> 后回 <ProbeMatch>，
// 里面告诉客户端 ONVIF SOAP 服务的 HTTP XAddr。
//
// 本实现只处理 ONVIF 的 NetworkVideoTransmitter 类型 Probe，其他类型一律忽略。

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace rtsp {

class WsDiscoveryResponder {
public:
    struct Config {
        // 设备 EndpointReference (urn:uuid:...), 每次启动自动生成
        std::string endpoint_uuid;
        // 对外宣告的 HTTP XAddr，例如 http://192.168.1.10:8080/onvif/device_service
        std::string xaddr;
        // Hardware/Scopes 元数据
        std::string scopes;  // 空格分隔的 onvif:// URI 列表
    };

    using ProbeReceivedCallback = std::function<void()>;  // 统计用，可选

    WsDiscoveryResponder();
    ~WsDiscoveryResponder();

    WsDiscoveryResponder(const WsDiscoveryResponder&) = delete;
    WsDiscoveryResponder& operator=(const WsDiscoveryResponder&) = delete;

    void setConfig(const Config& cfg);
    void setOnProbeReceived(ProbeReceivedCallback cb);
    void setOnMatchSent(ProbeReceivedCallback cb);

    // 启动：bind UDP + 加入多播组 + 开 listen 线程
    bool start();
    void stop();
    bool isRunning() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rtsp
