/**
 * Client RTP aggregation regression tests
 *
 * Verifies RtspClient can depacketize:
 * - H.264 STAP-A (type 24)
 * - H.265 AP (type 48)
 */

#include <rtsp-client/rtsp-client.h>
#include <rtsp-common/socket.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace rtsp;

namespace {

struct MockRtspServer {
    struct SendUnit {
        std::vector<uint8_t> payload;
        uint16_t seq = 1;
        uint32_t ts = 9000;
        bool marker = true;
    };

    uint16_t port;
    CodecType codec;
    uint8_t payload_type;
    std::vector<SendUnit> send_units;

    std::atomic<bool> running{false};
    std::thread thread;
    std::mutex sockets_mutex;
    std::unique_ptr<Socket> accepted_socket;
    Socket listen_socket;

    explicit MockRtspServer(uint16_t p, CodecType c, uint8_t pt, std::vector<uint8_t> payload)
        : port(p), codec(c), payload_type(pt) {
        SendUnit u;
        u.payload = std::move(payload);
        send_units.push_back(std::move(u));
    }

    MockRtspServer(uint16_t p, CodecType c, uint8_t pt, std::vector<SendUnit> units)
        : port(p), codec(c), payload_type(pt), send_units(std::move(units)) {}

    void start() {
        running = true;
        thread = std::thread([this]() { run(); });
    }

    void stop() {
        running = false;
        {
            std::lock_guard<std::mutex> lock(sockets_mutex);
            if (accepted_socket) {
                accepted_socket->close();
            }
            listen_socket.close();
        }
        if (thread.joinable()) {
            thread.detach();
        }
    }

    ~MockRtspServer() {
        stop();
    }

    static std::string readRequest(Socket& sock) {
        std::string buffer;
        uint8_t tmp[4096];
        while (true) {
            ssize_t n = sock.recv(tmp, sizeof(tmp), 3000);
            if (n <= 0) {
                return "";
            }
            buffer.append(reinterpret_cast<const char*>(tmp), static_cast<size_t>(n));
            if (buffer.find("\r\n\r\n") != std::string::npos) {
                return buffer;
            }
        }
    }

    static int parseCseq(const std::string& req) {
        std::regex cseq_re("CSeq:\\s*(\\d+)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(req, m, cseq_re)) {
            return std::stoi(m[1].str());
        }
        return 1;
    }

    static int parseClientRtpPort(const std::string& req) {
        std::regex port_re("client_port=(\\d+)-(\\d+)");
        std::smatch m;
        if (std::regex_search(req, m, port_re)) {
            return std::stoi(m[1].str());
        }
        return 0;
    }

    static std::string buildResponse(int cseq, const std::string& extra_headers, const std::string& body = "") {
        std::string resp = "RTSP/1.0 200 OK\r\n";
        resp += "CSeq: " + std::to_string(cseq) + "\r\n";
        resp += extra_headers;
        if (!body.empty()) {
            resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        resp += "\r\n";
        resp += body;
        return resp;
    }

    std::string buildSdp() const {
        if (codec == CodecType::H264) {
            return
                "v=0\r\n"
                "o=- 1 1 IN IP4 127.0.0.1\r\n"
                "s=Mock\r\n"
                "c=IN IP4 0.0.0.0\r\n"
                "t=0 0\r\n"
                "m=video 0 RTP/AVP 96\r\n"
                "a=rtpmap:96 H264/90000\r\n"
                "a=framesize:96 640-480\r\n"
                "a=control:stream\r\n";
        }

        return
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=Mock\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 97\r\n"
            "a=rtpmap:97 H265/90000\r\n"
            "a=framesize:97 640-480\r\n"
            "a=control:stream\r\n";
    }

    std::vector<uint8_t> buildRtpPacket(const SendUnit& unit, uint32_t ssrc) const {
        std::vector<uint8_t> pkt(12 + unit.payload.size());
        pkt[0] = 0x80;
        pkt[1] = static_cast<uint8_t>((unit.marker ? 0x80 : 0x00) | (payload_type & 0x7F));
        pkt[2] = static_cast<uint8_t>((unit.seq >> 8) & 0xFF);
        pkt[3] = static_cast<uint8_t>(unit.seq & 0xFF);
        pkt[4] = static_cast<uint8_t>((unit.ts >> 24) & 0xFF);
        pkt[5] = static_cast<uint8_t>((unit.ts >> 16) & 0xFF);
        pkt[6] = static_cast<uint8_t>((unit.ts >> 8) & 0xFF);
        pkt[7] = static_cast<uint8_t>(unit.ts & 0xFF);
        pkt[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
        pkt[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
        pkt[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
        pkt[11] = static_cast<uint8_t>(ssrc & 0xFF);
        memcpy(pkt.data() + 12, unit.payload.data(), unit.payload.size());
        return pkt;
    }

    void run() {
        if (!listen_socket.bind("127.0.0.1", port)) return;
        if (!listen_socket.listen()) return;

        auto client = listen_socket.accept();
        if (!client) return;
        {
            std::lock_guard<std::mutex> lock(sockets_mutex);
            accepted_socket = std::move(client);
        }

        std::string client_ip = accepted_socket->getPeerIp();
        int client_rtp_port = 0;
        std::string session = "12345678";

        while (running) {
            std::string req = readRequest(*accepted_socket);
            if (req.empty()) break;

            int cseq = parseCseq(req);
            if (req.find("DESCRIBE ") == 0) {
                std::string sdp = buildSdp();
                std::string resp = buildResponse(cseq, "Content-Type: application/sdp\r\n", sdp);
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
            } else if (req.find("SETUP ") == 0) {
                client_rtp_port = parseClientRtpPort(req);
                std::string transport = "Transport: RTP/AVP;unicast;client_port=" +
                                        std::to_string(client_rtp_port) + "-" + std::to_string(client_rtp_port + 1) +
                                        ";server_port=30000-30001\r\n";
                std::string resp = buildResponse(cseq, transport + "Session: " + session + "\r\n");
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
            } else if (req.find("PLAY ") == 0) {
                std::string resp = buildResponse(cseq, "Session: " + session + "\r\nRange: npt=0.000-\r\n");
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());

                if (client_rtp_port > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    Socket udp;
                    if (udp.bindUdp("0.0.0.0", 0)) {
                        for (const auto& unit : send_units) {
                            auto pkt = buildRtpPacket(unit, 0x11223344);
                            udp.sendTo(pkt.data(), pkt.size(), client_ip, static_cast<uint16_t>(client_rtp_port));
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                    }
                }
            } else if (req.find("TEARDOWN ") == 0) {
                std::string resp = buildResponse(cseq, "");
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
                break;
            } else {
                std::string resp = buildResponse(cseq, "");
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
            }
        }
    }
};

std::vector<uint8_t> createH264StapA() {
    // STAP-A hdr(type=24), then [size][nalu]...
    std::vector<uint8_t> p;
    const std::vector<uint8_t> nalu1 = {0x41, 0x01, 0x02};       // non-IDR
    const std::vector<uint8_t> nalu2 = {0x65, 0x88, 0x84, 0x21}; // IDR
    p.push_back(0x78);
    p.push_back(0x00);
    p.push_back(static_cast<uint8_t>(nalu1.size()));
    p.insert(p.end(), nalu1.begin(), nalu1.end());
    p.push_back(0x00);
    p.push_back(static_cast<uint8_t>(nalu2.size()));
    p.insert(p.end(), nalu2.begin(), nalu2.end());
    return p;
}

std::vector<uint8_t> createH265Ap() {
    // AP hdr(type=48), then [size][nalu]...
    std::vector<uint8_t> p;
    const std::vector<uint8_t> nalu1 = {0x02, 0x01, 0x11};       // type=1
    const std::vector<uint8_t> nalu2 = {0x26, 0x01, 0x99, 0x88}; // type=19 IRAP
    p.push_back(0x60); // (48 << 1)
    p.push_back(0x01); // TID+1
    p.push_back(0x00);
    p.push_back(static_cast<uint8_t>(nalu1.size()));
    p.insert(p.end(), nalu1.begin(), nalu1.end());
    p.push_back(0x00);
    p.push_back(static_cast<uint8_t>(nalu2.size()));
    p.insert(p.end(), nalu2.begin(), nalu2.end());
    return p;
}

std::vector<uint8_t> createH264StapB() {
    // STAP-B hdr(type=25), DON(2), then [size][nalu]...
    std::vector<uint8_t> p;
    const std::vector<uint8_t> nalu1 = {0x41, 0x77, 0x66};
    const std::vector<uint8_t> nalu2 = {0x65, 0x55, 0x44, 0x33};
    p.push_back(0x79);
    p.push_back(0x00); // DON
    p.push_back(0x01); // DON
    p.push_back(0x00);
    p.push_back(static_cast<uint8_t>(nalu1.size()));
    p.insert(p.end(), nalu1.begin(), nalu1.end());
    p.push_back(0x00);
    p.push_back(static_cast<uint8_t>(nalu2.size()));
    p.insert(p.end(), nalu2.begin(), nalu2.end());
    return p;
}

std::vector<MockRtspServer::SendUnit> createH265FuLossThenRecovery() {
    // Frame #1 (ts=9000): start + missing middle + end -> should be dropped.
    // Frame #2 (ts=12000): start + end -> should be decoded.
    std::vector<MockRtspServer::SendUnit> units;

    MockRtspServer::SendUnit u1;
    u1.seq = 1;
    u1.ts = 9000;
    u1.marker = false;
    u1.payload = {0x62, 0x01, 0x93, 0xAA, 0xBB}; // FU start, type=19
    units.push_back(u1);

    MockRtspServer::SendUnit u2;
    u2.seq = 3; // seq=2 intentionally missing
    u2.ts = 9000;
    u2.marker = true;
    u2.payload = {0x62, 0x01, 0x53, 0xCC, 0xDD}; // FU end
    units.push_back(u2);

    MockRtspServer::SendUnit u3;
    u3.seq = 4;
    u3.ts = 12000;
    u3.marker = false;
    u3.payload = {0x62, 0x01, 0x93, 0x11, 0x22}; // FU start
    units.push_back(u3);

    MockRtspServer::SendUnit u4;
    u4.seq = 5;
    u4.ts = 12000;
    u4.marker = true;
    u4.payload = {0x62, 0x01, 0x53, 0x33, 0x44}; // FU end
    units.push_back(u4);

    return units;
}

std::vector<MockRtspServer::SendUnit> createH265FuOutOfOrder() {
    // Same frame, packets sent as 1(start), 3(end), 2(middle).
    std::vector<MockRtspServer::SendUnit> units;

    MockRtspServer::SendUnit u1;
    u1.seq = 1;
    u1.ts = 15000;
    u1.marker = false;
    u1.payload = {0x62, 0x01, 0x93, 0x10, 0x11}; // FU start, type=19
    units.push_back(u1);

    MockRtspServer::SendUnit u3;
    u3.seq = 3;
    u3.ts = 15000;
    u3.marker = true;
    u3.payload = {0x62, 0x01, 0x53, 0x30, 0x31}; // FU end
    units.push_back(u3);

    MockRtspServer::SendUnit u2;
    u2.seq = 2;
    u2.ts = 15000;
    u2.marker = false;
    u2.payload = {0x62, 0x01, 0x13, 0x20, 0x21}; // FU middle
    units.push_back(u2);

    return units;
}

void test_h264_stap_a_receive() {
    std::cout << "Testing H.264 STAP-A depacketization..." << std::endl;

    MockRtspServer server(19554, CodecType::H264, 96, createH264StapA());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RtspClient client;
    assert(client.open("rtsp://127.0.0.1:19554/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    VideoFrame frame{};
    assert(client.receiveFrame(frame, 2000));
    assert(frame.codec == CodecType::H264);
    assert(frame.type == FrameType::IDR);
    assert(frame.size >= 8);
    assert(frame.data[0] == 0x00 && frame.data[1] == 0x00 && frame.data[2] == 0x00 && frame.data[3] == 0x01);

    client.close();
    server.stop();

    std::cout << "  H.264 STAP-A tests passed!" << std::endl;
}

void test_h265_ap_receive() {
    std::cout << "Testing H.265 AP depacketization..." << std::endl;

    MockRtspServer server(19555, CodecType::H265, 97, createH265Ap());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RtspClient client;
    assert(client.open("rtsp://127.0.0.1:19555/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    VideoFrame frame{};
    assert(client.receiveFrame(frame, 2000));
    assert(frame.codec == CodecType::H265);
    assert(frame.type == FrameType::IDR);
    assert(frame.size >= 8);
    assert(frame.data[0] == 0x00 && frame.data[1] == 0x00 && frame.data[2] == 0x00 && frame.data[3] == 0x01);

    client.close();
    server.stop();

    std::cout << "  H.265 AP tests passed!" << std::endl;
}

void test_h264_stap_b_receive() {
    std::cout << "Testing H.264 STAP-B depacketization..." << std::endl;

    MockRtspServer server(19556, CodecType::H264, 96, createH264StapB());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RtspClient client;
    assert(client.open("rtsp://127.0.0.1:19556/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    VideoFrame frame{};
    assert(client.receiveFrame(frame, 2000));
    assert(frame.codec == CodecType::H264);
    assert(frame.type == FrameType::IDR);
    assert(frame.size >= 8);
    assert(frame.data[0] == 0x00 && frame.data[1] == 0x00 && frame.data[2] == 0x00 && frame.data[3] == 0x01);

    client.close();
    server.stop();

    std::cout << "  H.264 STAP-B tests passed!" << std::endl;
}

void test_h265_fu_loss_resync() {
    std::cout << "Testing H.265 FU loss resync..." << std::endl;

    MockRtspServer server(19557, CodecType::H265, 97, createH265FuLossThenRecovery());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RtspClient client;
    assert(client.open("rtsp://127.0.0.1:19557/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    VideoFrame frame{};
    assert(client.receiveFrame(frame, 2000));
    assert(frame.codec == CodecType::H265);
    assert(frame.type == FrameType::IDR);
    assert(frame.size >= 10);
    assert(frame.data[0] == 0x00 && frame.data[1] == 0x00 && frame.data[2] == 0x00 && frame.data[3] == 0x01);
    // Reconstructed NAL header should be 0x26 0x01 for type=19 stream.
    assert(frame.data[4] == 0x26 && frame.data[5] == 0x01);

    client.close();
    server.stop();

    std::cout << "  H.265 FU loss resync tests passed!" << std::endl;
}

void test_h265_fu_out_of_order_reorder() {
    std::cout << "Testing H.265 FU out-of-order reorder..." << std::endl;

    MockRtspServer server(19558, CodecType::H265, 97, createH265FuOutOfOrder());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RtspClient client;
    RtspClientConfig cfg;
    cfg.jitter_buffer_packets = 16;
    client.setConfig(cfg);
    assert(client.open("rtsp://127.0.0.1:19558/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    VideoFrame frame{};
    assert(client.receiveFrame(frame, 2000));
    assert(frame.codec == CodecType::H265);
    assert(frame.type == FrameType::IDR);
    assert(frame.size >= 12);
    assert(frame.data[0] == 0x00 && frame.data[1] == 0x00 && frame.data[2] == 0x00 && frame.data[3] == 0x01);
    assert(frame.data[4] == 0x26 && frame.data[5] == 0x01);

    client.close();
    server.stop();

    std::cout << "  H.265 FU out-of-order reorder tests passed!" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Client RTP Aggregation Tests ===" << std::endl;

    try {
        test_h264_stap_a_receive();
        test_h265_ap_receive();
        test_h264_stap_b_receive();
        test_h265_fu_loss_resync();
        test_h265_fu_out_of_order_reorder();
        std::cout << "\n=== All Client RTP Aggregation Tests Passed! ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
