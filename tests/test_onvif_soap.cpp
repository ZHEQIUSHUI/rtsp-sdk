// 端到端：起 RTSP server + ONVIF daemon，用 TCP 客户端打 SOAP HTTP POST，
// 断言 GetProfiles / GetStreamUri / 鉴权路径都工作。
//
// 避免拉依赖，测试直接用 Socket 发裸 HTTP，不使用 httplib 的 Client。
#include <rtsp-server/rtsp-server.h>
#include <rtsp-onvif/rtsp-onvif.h>
#include <rtsp-common/socket.h>
#include <rtsp-common/common.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace rtsp;
using namespace std::chrono_literals;

namespace {

std::string sendHttpPost(const std::string& host, uint16_t port,
                         const std::string& path, const std::string& soap_xml,
                         const std::string& soap_action,
                         int* out_status = nullptr) {
    Socket s;
    if (!s.connect(host, port, 3000)) return {};

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/soap+xml; charset=utf-8\r\n"
        << "SOAPAction: \"" << soap_action << "\"\r\n"
        << "Content-Length: " << soap_xml.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << soap_xml;
    const std::string w = req.str();
    if (s.send(reinterpret_cast<const uint8_t*>(w.data()), w.size()) <= 0) return {};

    std::string resp;
    uint8_t buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = s.recv(buf, sizeof(buf), 500);
        if (n > 0) resp.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
        else if (n == 0) break;   // peer closed
    }
    if (out_status) {
        // parse "HTTP/1.1 200 OK"
        if (resp.size() >= 12 && resp.compare(0, 5, "HTTP/") == 0) {
            *out_status = std::atoi(resp.c_str() + 9);
        } else {
            *out_status = 0;
        }
    }
    return resp;
}

std::string isoNowUtc(int offset_seconds) {
    std::time_t t = std::time(nullptr) + offset_seconds;
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

std::string makeSecurityHeader(const std::string& user, const std::string& pass) {
    // nonce：8 字节固定值即可（本测试里使用一次，不会重放）
    const uint8_t nonce_raw[8] = {1,2,3,4,5,6,7,8};
    const std::string nonce_b64 =
        base64Encode(nonce_raw, sizeof(nonce_raw));
    const std::string created = isoNowUtc(0);

    // SHA1(nonce_raw + created + password)
    std::vector<uint8_t> input;
    input.insert(input.end(), nonce_raw, nonce_raw + sizeof(nonce_raw));
    input.insert(input.end(), created.begin(), created.end());
    input.insert(input.end(), pass.begin(), pass.end());

    // 用 wsse_auth.cpp 暴露的 sha1Raw（通过 extern 声明）
    extern std::string sha1Raw_in_rtsp(const uint8_t*, size_t);  // placeholder; 见下
    // 本测试直接复用库内的 SHA1，但它在 onvif 模块的 anonymous 命名空间里。
    // 简单起见，我们这里手写一遍（与 wsse_auth.cpp 里的实现一致）。
    // 避免测试侵入库实现，用一个最小自包含版本。
    auto rol = [](uint32_t x, uint32_t n){ return (x<<n)|(x>>(32-n)); };
    uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;
    std::vector<uint8_t> msg = input;
    uint64_t bit_len = uint64_t(msg.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(uint8_t((bit_len >> (i*8)) & 0xFF));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(msg[off+i*4])<<24)|(uint32_t(msg[off+i*4+1])<<16)
                 | (uint32_t(msg[off+i*4+2])<<8)|uint32_t(msg[off+i*4+3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i<20){f=(b&c)|((~b)&d); k=0x5A827999;}
            else if(i<40){f=b^c^d; k=0x6ED9EBA1;}
            else if(i<60){f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC;}
            else{f=b^c^d; k=0xCA62C1D6;}
            uint32_t t = rol(a,5)+f+e+k+w[i];
            e=d;d=c;c=rol(b,30);b=a;a=t;
        }
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
    }
    uint8_t digest[20];
    uint32_t hs[5]={h0,h1,h2,h3,h4};
    for (int i=0;i<5;++i){
        digest[i*4]=uint8_t(hs[i]>>24);
        digest[i*4+1]=uint8_t(hs[i]>>16);
        digest[i*4+2]=uint8_t(hs[i]>>8);
        digest[i*4+3]=uint8_t(hs[i]);
    }
    const std::string digest_b64 = base64Encode(digest, 20);

    std::ostringstream oss;
    oss << "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
        <<   "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        <<   "<wsse:UsernameToken>"
        <<     "<wsse:Username>" << user << "</wsse:Username>"
        <<     "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
        <<        digest_b64 << "</wsse:Password>"
        <<     "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"
        <<        nonce_b64 << "</wsse:Nonce>"
        <<     "<wsu:Created>" << created << "</wsu:Created>"
        <<   "</wsse:UsernameToken>"
        << "</wsse:Security>";
    return oss.str();
}

std::string wrapEnvelope(const std::string& header_inner, const std::string& body_inner) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
        <<  "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
        <<  "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        << "<soap:Header>" << header_inner << "</soap:Header>"
        << "<soap:Body>" << body_inner << "</soap:Body>"
        << "</soap:Envelope>";
    return oss.str();
}

}  // namespace

int main() {
    const uint16_t kRtspPort = 18811;
    const uint16_t kHttpPort = 18812;
    RtspServer srv;
    srv.init("0.0.0.0", kRtspPort);
    PathConfig pc;
    pc.path = "/live/stream";
    pc.codec = CodecType::H264;
    pc.width = 1920; pc.height = 1080; pc.fps = 25;
    srv.addPath(pc);
    PathConfig pc2;
    pc2.path = "/live/sub";
    pc2.codec = CodecType::H265;
    pc2.width = 640; pc2.height = 360; pc2.fps = 15;
    srv.addPath(pc2);
    if (!srv.start()) { std::cerr << "SKIP: rtsp start fail" << std::endl; return 0; }

    OnvifDaemon d;
    OnvifDaemonConfig cfg;
    cfg.http_port = kHttpPort;
    cfg.rtsp_port = kRtspPort;
    cfg.announce_host = "127.0.0.1";          // 测试固定 host
    cfg.announce_rtsp_host = "127.0.0.1";
    cfg.enable_ws_discovery = false;          // 本测试不需要多播
    cfg.auth_username = "admin";
    cfg.auth_password = "secret123";
    d.attachServer(&srv);
    d.setConfig(cfg);
    if (!d.start()) { std::cerr << "FAIL: onvif start" << std::endl; srv.stop(); return 1; }

    std::this_thread::sleep_for(200ms);

    // ---------- 1. 匿名 action: GetCapabilities 允许不鉴权 ----------
    {
        const std::string body = wrapEnvelope("",
            "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>");
        int status = 0;
        const std::string resp = sendHttpPost("127.0.0.1", kHttpPort, "/onvif/device_service",
                                              body,
                                              "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                                              &status);
        assert(status == 200 && "anonymous GetCapabilities must succeed");
        assert(resp.find("GetCapabilitiesResponse") != std::string::npos);
        assert(resp.find(":" + std::to_string(kHttpPort)) != std::string::npos);
    }

    // ---------- 2. 需鉴权的 action 无 UsernameToken: 401 ----------
    {
        const std::string body = wrapEnvelope("",
            "<tds:GetDeviceInformation/>");
        int status = 0;
        sendHttpPost("127.0.0.1", kHttpPort, "/onvif/device_service", body,
                     "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation", &status);
        assert(status == 401 && "protected action must 401 without auth");
    }

    // ---------- 3. 正确鉴权: GetDeviceInformation 成功 ----------
    {
        const std::string sec = makeSecurityHeader("admin", "secret123");
        const std::string body = wrapEnvelope(sec, "<tds:GetDeviceInformation/>");
        int status = 0;
        const std::string resp = sendHttpPost("127.0.0.1", kHttpPort, "/onvif/device_service", body,
                     "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation", &status);
        assert(status == 200 && "authenticated GetDeviceInformation must succeed");
        assert(resp.find("GetDeviceInformationResponse") != std::string::npos);
    }

    // ---------- 4. 错误密码: 401 ----------
    {
        const std::string sec = makeSecurityHeader("admin", "wrongpw");
        const std::string body = wrapEnvelope(sec, "<tds:GetDeviceInformation/>");
        int status = 0;
        sendHttpPost("127.0.0.1", kHttpPort, "/onvif/device_service", body,
                     "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation", &status);
        assert(status == 401 && "wrong password must fail auth");
    }

    // ---------- 5. Media: GetProfiles ----------
    {
        const std::string sec = makeSecurityHeader("admin", "secret123");
        const std::string body = wrapEnvelope(sec, "<trt:GetProfiles/>");
        int status = 0;
        const std::string resp = sendHttpPost("127.0.0.1", kHttpPort, "/onvif/media_service", body,
                     "http://www.onvif.org/ver10/media/wsdl/GetProfiles", &status);
        assert(status == 200);
        // 两个 path 都应该出现
        assert(resp.find("livestream") != std::string::npos);
        assert(resp.find("livesub") != std::string::npos);
        // H265 profile 应反映
        assert(resp.find("H265") != std::string::npos);
    }

    // ---------- 6. Media: GetStreamUri 对特定 profile ----------
    {
        const std::string sec = makeSecurityHeader("admin", "secret123");
        const std::string body = wrapEnvelope(sec,
            "<trt:GetStreamUri>"
            "<trt:StreamSetup><trt:Stream>RTP-Unicast</trt:Stream>"
            "<trt:Transport><trt:Protocol>RTSP</trt:Protocol></trt:Transport>"
            "</trt:StreamSetup>"
            "<trt:ProfileToken>livestream</trt:ProfileToken>"
            "</trt:GetStreamUri>");
        int status = 0;
        const std::string resp = sendHttpPost("127.0.0.1", kHttpPort, "/onvif/media_service", body,
                     "http://www.onvif.org/ver10/media/wsdl/GetStreamUri", &status);
        assert(status == 200);
        const std::string expected_uri = "rtsp://127.0.0.1:" + std::to_string(kRtspPort) + "/live/stream";
        assert(resp.find(expected_uri) != std::string::npos && "StreamUri must reflect attached server path");
    }

    // ---------- 7. Media: GetStreamUri 对 sub profile ----------
    {
        const std::string sec = makeSecurityHeader("admin", "secret123");
        const std::string body = wrapEnvelope(sec,
            "<trt:GetStreamUri>"
            "<trt:ProfileToken>livesub</trt:ProfileToken>"
            "</trt:GetStreamUri>");
        int status = 0;
        const std::string resp = sendHttpPost("127.0.0.1", kHttpPort, "/onvif/media_service", body,
                     "http://www.onvif.org/ver10/media/wsdl/GetStreamUri", &status);
        assert(status == 200);
        const std::string expected_uri = "rtsp://127.0.0.1:" + std::to_string(kRtspPort) + "/live/sub";
        assert(resp.find(expected_uri) != std::string::npos);
    }

    const auto stats = d.getStats();
    std::cout << "soap reqs=" << stats.soap_requests_total
              << " auth_fail=" << stats.soap_auth_failures << std::endl;
    assert(stats.soap_requests_total >= 7);
    assert(stats.soap_auth_failures >= 2);

    d.stop();
    srv.stop();
    std::cout << "onvif soap + auth test passed" << std::endl;
    return 0;
}
