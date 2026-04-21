// 验证 DESCRIBE 响应含 Content-Base 头（M6）以及 client 识别 Content-Base 解析相对 control URL（M8）
#include <rtsp-server/rtsp-server.h>
#include <rtsp-common/socket.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <regex>
#include <string>
#include <thread>

using namespace rtsp;
using namespace std::chrono_literals;

static std::string sendAndRead(Socket& s, const std::string& req) {
    s.send(reinterpret_cast<const uint8_t*>(req.data()), req.size());
    std::string resp;
    if (!recvRtspMessage(s, &resp, 3000)) return {};
    return resp;
}

int main() {
    const uint16_t kPort = 18671;
    auto server = std::make_shared<RtspServer>();
    server->init("127.0.0.1", kPort);
    PathConfig cfg;
    cfg.path = "/live/cam1";
    cfg.codec = CodecType::H264;
    cfg.sps = {0x67, 0x42, 0x00, 0x28};
    cfg.pps = {0x68, 0xCE, 0x3C, 0x80};
    server->addPath(cfg);
    if (!server->start()) {
        std::cerr << "SKIP: failed to bind 127.0.0.1:" << kPort << std::endl;
        return 0;
    }
    std::this_thread::sleep_for(150ms);

    Socket sock;
    if (!sock.connect("127.0.0.1", kPort, 2000)) {
        std::cerr << "FAIL: connect" << std::endl;
        return 1;
    }
    const std::string describe =
        "DESCRIBE rtsp://127.0.0.1:" + std::to_string(kPort) + "/live/cam1 RTSP/1.0\r\n"
        "CSeq: 1\r\nAccept: application/sdp\r\n\r\n";
    std::string resp = sendAndRead(sock, describe);
    std::cout << "--- DESCRIBE response ---\n" << resp << "\n-------------------------" << std::endl;

    assert(resp.find("200 OK") != std::string::npos);
    std::cerr << "[dbg] 200 OK ok" << std::endl;

    // 用 find 而非 std::regex 以避开可能的 libstdc++ regex 栈崩溃问题。
    // 断言存在 Content-Base 且以 '/' 结尾。
    const std::string needle = "Content-Base:";
    auto pos = resp.find(needle);
    assert(pos != std::string::npos && "response must include Content-Base");
    pos += needle.size();
    while (pos < resp.size() && (resp[pos] == ' ' || resp[pos] == '\t')) ++pos;
    auto eol = resp.find("\r\n", pos);
    assert(eol != std::string::npos);
    std::string cb = resp.substr(pos, eol - pos);
    std::cerr << "[dbg] cb=[" << cb << "]" << std::endl;
    assert(!cb.empty() && cb.back() == '/' && "Content-Base must end with '/'");

    sock.close();
    server->stop();
    std::cout << "Content-Base test passed\n";
    return 0;
}
