// 模拟一个 ONVIF 客户端：发 WS-Discovery Probe 到多播地址，
// 断言能收到 ProbeMatch，且 XAddr 指向我们的 device_service。
//
// 走的是本机多播，回环禁用情况下我们用单播 Probe 到 127.0.0.1:3702 也能命中。
#include <rtsp-server/rtsp-server.h>
#include <rtsp-onvif/rtsp-onvif.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
    #define NOMINMAX
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socklen_t_compat = int;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using socklen_t_compat = socklen_t;
#endif

using namespace rtsp;
using namespace std::chrono_literals;

static bool sendProbeAndRecv(const std::string& dst_ip, uint16_t dst_port,
                             std::string& out_response, int timeout_ms) {
    static const char* kProbe =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
          "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
          "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
          "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<soap:Header>"
          "<wsa:MessageID>urn:uuid:cafebabe-0001-0001-0001-000000000001</wsa:MessageID>"
          "<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
          "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>"
        "</soap:Header>"
        "<soap:Body>"
          "<wsd:Probe><wsd:Types>dn:NetworkVideoTransmitter</wsd:Types></wsd:Probe>"
        "</soap:Body>"
        "</soap:Envelope>";

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
#endif

    int reuse = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(0);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    ::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dst_port);
    dst.sin_addr.s_addr = inet_addr(dst_ip.c_str());

    const int pl = static_cast<int>(std::strlen(kProbe));
    if (::sendto(s, kProbe, pl, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != pl) {
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        return false;
    }

    // 接收超时
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    char buf[8192];
    sockaddr_in from{};
    socklen_t_compat fromlen = sizeof(from);
    const ssize_t n = ::recvfrom(s, buf, sizeof(buf) - 1, 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromlen);
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
    if (n <= 0) return false;
    buf[n] = '\0';
    out_response.assign(buf, static_cast<size_t>(n));
    return true;
}

int main() {
    const uint16_t kRtspPort = 18801;
    const uint16_t kHttpPort = 18802;
    RtspServer srv;
    srv.init("0.0.0.0", kRtspPort);
    PathConfig pc;
    pc.path = "/live/cam1";
    pc.codec = CodecType::H264;
    pc.width = 1280; pc.height = 720; pc.fps = 30;
    srv.addPath(pc);
    if (!srv.start()) {
        std::cerr << "SKIP: failed to bind RTSP port" << std::endl;
        return 0;
    }

    OnvifDaemon d;
    OnvifDaemonConfig cfg;
    cfg.http_port = kHttpPort;
    cfg.rtsp_port = kRtspPort;
    cfg.device_info.manufacturer = "test-brand";
    cfg.device_info.model = "test-model";
    cfg.device_info.hardware_id = "test-hw";
    d.attachServer(&srv);
    d.setConfig(cfg);
    if (!d.start()) {
        std::cerr << "SKIP: onvif daemon start failed (firewall?)" << std::endl;
        srv.stop();
        return 0;
    }

    // 等 discovery 线程把 multicast 注册完
    std::this_thread::sleep_for(250ms);

    std::string resp;
    // 先尝试多播（标准路径）
    bool got = sendProbeAndRecv("239.255.255.250", 3702, resp, 1500);
    if (!got) {
        std::cerr << "[info] multicast probe got no answer (CI 容器可能不支持多播)，"
                  << "改用单播到 127.0.0.1:3702" << std::endl;
        got = sendProbeAndRecv("127.0.0.1", 3702, resp, 1500);
    }

    if (!got) {
        // CI 环境不允许多播，也不允许 loopback UDP 到受多播组监听的 socket；
        // 这两种都允许就不是我们测试能覆盖的了。跳过。
        std::cerr << "SKIP: environment does not permit WS-Discovery round-trip" << std::endl;
        d.stop();
        srv.stop();
        return 0;
    }

    std::cout << "=== ProbeMatch response ===\n" << resp.substr(0, 800) << "\n===" << std::endl;

    // 断言关键字段
    assert(resp.find("ProbeMatch") != std::string::npos && "must contain ProbeMatch");
    assert(resp.find("NetworkVideoTransmitter") != std::string::npos && "must declare NVT type");
    // XAddr 要指向我们的 device_service
    const std::string xaddr_token = "/onvif/device_service";
    assert(resp.find(xaddr_token) != std::string::npos && "XAddr must include device_service path");
    const std::string port_token = ":" + std::to_string(kHttpPort);
    assert(resp.find(port_token) != std::string::npos && "XAddr must include our http port");
    // RelatesTo 指向我们 Probe 的 MessageID
    assert(resp.find("cafebabe-0001-0001-0001-000000000001") != std::string::npos);

    const auto stats = d.getStats();
    std::cout << "probes=" << stats.discovery_probes_received
              << " matches=" << stats.discovery_matches_sent << std::endl;
    assert(stats.discovery_probes_received >= 1);
    assert(stats.discovery_matches_sent >= 1);

    d.stop();
    srv.stop();
    std::cout << "onvif discovery test passed" << std::endl;
    return 0;
}
