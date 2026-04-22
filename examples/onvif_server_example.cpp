// RTSP + ONVIF 一站式示例：启动 RTSP server，同时起 ONVIF 发现与 SOAP 服务，
// 让局域网内 ONVIF Device Manager / NVR / VMS 扫到并取到流地址。
//
// 用法：
//   onvif_server_example [--port N] [--path /live/stream] [--auth user:pass] [--http N]
//
// 测试：启动后在 ONVIF Device Manager 或类似工具中"扫描"，应能看到设备；
//       选定 → 拿到的 RTSP URL 可直接用 ffmpeg/VLC 播放。

#include <rtsp-server/rtsp-server.h>
#include <rtsp-onvif/rtsp-onvif.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace rtsp;

static const uint8_t EXAMPLE_SPS[] = {0x67,0x42,0xC0,0x1F,0xD9,0x00,0x78,0x02,0x27,0xE5,
                                      0xC0,0x44,0x00,0x00,0x03,0x00,0x04,0x00,0x00,0x03,
                                      0x00,0xF0,0x3C,0x60,0xC6,0x58};
static const uint8_t EXAMPLE_PPS[] = {0x68,0xCE,0x3C,0x80};

static std::atomic<bool> g_running{true};
static void onSig(int) { g_running = false; }

int main(int argc, char** argv) {
    uint16_t rtsp_port = 8554;
    std::string path   = "/live/stream";
    uint16_t http_port = 8080;
    std::string auth_user, auth_pass;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)      rtsp_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--path" && i + 1 < argc) path = argv[++i];
        else if (a == "--http" && i + 1 < argc) http_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--auth" && i + 1 < argc) {
            std::string ua = argv[++i];
            auto p = ua.find(':');
            if (p != std::string::npos) { auth_user = ua.substr(0, p); auth_pass = ua.substr(p + 1); }
        }
    }

    std::signal(SIGINT, onSig);

    RtspServer server;
    server.init("0.0.0.0", rtsp_port);
    PathConfig pcfg;
    pcfg.path = path;
    pcfg.codec = CodecType::H264;
    pcfg.width = 640;
    pcfg.height = 480;
    pcfg.fps = 30;
    pcfg.sps.assign(EXAMPLE_SPS, EXAMPLE_SPS + sizeof(EXAMPLE_SPS));
    pcfg.pps.assign(EXAMPLE_PPS, EXAMPLE_PPS + sizeof(EXAMPLE_PPS));
    server.addPath(pcfg);
    if (!server.start()) {
        std::cerr << "RTSP server start failed" << std::endl;
        return 1;
    }

    OnvifDaemon onvif;
    OnvifDaemonConfig ocfg;
    ocfg.http_port = http_port;
    ocfg.rtsp_port = rtsp_port;
    ocfg.device_info.manufacturer = "rtsp-sdk";
    ocfg.device_info.model        = "virtual-cam";
    ocfg.device_info.firmware     = "1.0.0";
    ocfg.device_info.serial       = "SN-EXAMPLE-0001";
    ocfg.device_info.hardware_id  = "hw-v1";
    if (!auth_user.empty()) {
        ocfg.auth_username = auth_user;
        ocfg.auth_password = auth_pass;
    }
    onvif.attachServer(&server);
    onvif.setConfig(ocfg);
    if (!onvif.start()) {
        std::cerr << "ONVIF daemon start failed" << std::endl;
        server.stop();
        return 1;
    }

    std::cout << "=== Running ===" << std::endl;
    std::cout << "RTSP URL:   rtsp://<host>:" << rtsp_port << path << std::endl;
    std::cout << "ONVIF HTTP: http://<host>:" << http_port << "/onvif/device_service" << std::endl;
    if (!auth_user.empty()) {
        std::cout << "ONVIF auth: " << auth_user << " (WS-Security PasswordDigest)" << std::endl;
    }
    std::cout << "Ctrl+C to stop." << std::endl;

    // 简单推流器：重复发送 SPS+PPS+IDR
    std::vector<uint8_t> frame;
    auto pushOne = [&](uint64_t pts) {
        frame.clear();
        frame.insert(frame.end(), {0x00,0x00,0x00,0x01});
        frame.insert(frame.end(), EXAMPLE_SPS, EXAMPLE_SPS + sizeof(EXAMPLE_SPS));
        frame.insert(frame.end(), {0x00,0x00,0x00,0x01});
        frame.insert(frame.end(), EXAMPLE_PPS, EXAMPLE_PPS + sizeof(EXAMPLE_PPS));
        frame.insert(frame.end(), {0x00,0x00,0x00,0x01, 0x65, 0x88, 0x80, 0x00});
        server.pushH264Data(path, frame.data(), frame.size(), pts, true);
    };

    uint64_t pts = 0;
    while (g_running.load()) {
        pushOne(pts);
        pts += 33;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    const auto s = onvif.getStats();
    std::cout << "\nONVIF stats: probes=" << s.discovery_probes_received
              << " matches=" << s.discovery_matches_sent
              << " soap_reqs=" << s.soap_requests_total
              << " soap_auth_fail=" << s.soap_auth_failures << std::endl;

    onvif.stop();
    server.stop();
    return 0;
}
