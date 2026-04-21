// 验证 server 发出的 RTP 与 RTCP SR 携带同一 SSRC（C4 回归）
#include <rtsp-server/rtsp-server.h>
#include <rtsp-common/socket.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
#include <cstring>

using namespace rtsp;
using namespace std::chrono_literals;

static uint32_t ssrcFromRtp(const uint8_t* data, size_t len) {
    assert(len >= 12);
    return (uint32_t(data[8]) << 24) | (uint32_t(data[9]) << 16) |
           (uint32_t(data[10]) << 8) | uint32_t(data[11]);
}

static uint32_t ssrcFromRtcpSr(const uint8_t* data, size_t len) {
    assert(len >= 8 && data[1] == 200 /* PT=SR */);
    return (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) |
           (uint32_t(data[6]) << 8) | uint32_t(data[7]);
}

int main() {
    const uint16_t kPort = 18681;
    auto server = std::make_shared<RtspServer>();
    server->init("127.0.0.1", kPort);
    PathConfig cfg;
    cfg.path = "/live";
    cfg.codec = CodecType::H264;
    cfg.sps = {0x67, 0x42, 0x00, 0x28};
    cfg.pps = {0x68, 0xCE, 0x3C, 0x80};
    server->addPath(cfg);
    if (!server->start()) {
        std::cerr << "SKIP: bind failed" << std::endl;
        return 0;
    }
    std::this_thread::sleep_for(150ms);

    // 自建 UDP pair 作为客户端
    Socket udp_rtp, udp_rtcp;
    if (!udp_rtp.bindUdp("127.0.0.1", 18690) ||
        !udp_rtcp.bindUdp("127.0.0.1", 18691)) {
        std::cerr << "SKIP: udp bind failed" << std::endl;
        return 0;
    }

    Socket ctrl;
    if (!ctrl.connect("127.0.0.1", kPort, 2000)) { std::cerr << "FAIL connect" << std::endl; return 1; }

    auto sendReq = [&](const std::string& req) -> std::string {
        ctrl.send(reinterpret_cast<const uint8_t*>(req.data()), req.size());
        std::string resp; recvRtspMessage(ctrl, &resp, 3000);
        return resp;
    };

    sendReq("DESCRIBE rtsp://127.0.0.1:" + std::to_string(kPort) + "/live RTSP/1.0\r\n"
            "CSeq: 1\r\nAccept: application/sdp\r\n\r\n");
    auto setup_resp = sendReq(
        "SETUP rtsp://127.0.0.1:" + std::to_string(kPort) + "/live/stream RTSP/1.0\r\n"
        "CSeq: 2\r\nTransport: RTP/AVP;unicast;client_port=18690-18691\r\n\r\n");
    std::smatch m;
    std::regex sid_re("Session:\\s*([^;\\r\\n]+)");
    if (!std::regex_search(setup_resp, m, sid_re)) {
        std::cerr << "FAIL: no Session in SETUP response\n" << setup_resp << std::endl;
        return 1;
    }
    std::string sid = m[1].str();
    sendReq("PLAY rtsp://127.0.0.1:" + std::to_string(kPort) + "/live RTSP/1.0\r\n"
            "CSeq: 3\r\nSession: " + sid + "\r\n\r\n");

    // 推帧 150 帧（>100 触发 SR）
    std::vector<uint8_t> nalu;
    nalu.insert(nalu.end(), {0x00, 0x00, 0x00, 0x01, 0x67});
    nalu.insert(nalu.end(), 30, 0x42);
    nalu.insert(nalu.end(), {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80});
    nalu.insert(nalu.end(), {0x00, 0x00, 0x00, 0x01, 0x65});
    nalu.insert(nalu.end(), 500, 0x80);

    uint32_t rtp_ssrc = 0;
    uint32_t rtcp_ssrc = 0;
    for (int i = 0; i < 400 && rtcp_ssrc == 0; ++i) {
        server->pushH264Data("/live", nalu.data(), nalu.size(), i * 33, i % 30 == 0);
        std::this_thread::sleep_for(10ms);

        // 读所有可读的 RTP / RTCP
        for (int j = 0; j < 5; ++j) {
            if (udp_rtp.waitReadable(0) == 1) {
                uint8_t buf[2048]; std::string fip; uint16_t fport = 0;
                ssize_t n = udp_rtp.recvFrom(buf, sizeof(buf), fip, fport);
                if (n >= 12 && rtp_ssrc == 0) rtp_ssrc = ssrcFromRtp(buf, n);
            } else break;
        }
        for (int j = 0; j < 5; ++j) {
            if (udp_rtcp.waitReadable(0) == 1) {
                uint8_t buf[2048]; std::string fip; uint16_t fport = 0;
                ssize_t n = udp_rtcp.recvFrom(buf, sizeof(buf), fip, fport);
                if (n >= 28 && buf[1] == 200) { rtcp_ssrc = ssrcFromRtcpSr(buf, n); break; }
            } else break;
        }
    }

    std::cout << std::hex << "rtp_ssrc=0x" << rtp_ssrc
              << " rtcp_ssrc=0x" << rtcp_ssrc << std::dec << std::endl;

    assert(rtp_ssrc != 0 && "must receive at least one RTP");
    assert(rtcp_ssrc != 0 && "must receive at least one RTCP SR");
    assert(rtp_ssrc == rtcp_ssrc && "RTP and RTCP SR SSRC must match");

    server->stop();
    std::cout << "RTCP SSRC match test passed\n";
    return 0;
}
