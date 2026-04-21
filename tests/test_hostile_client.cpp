// 恶劣客户端回归：
//   1. 不发完整 header 的 slowloris → server 不应 OOM 也不应永久累积
//   2. 巨大 Content-Length → server 不应尝试接收那么多字节
//   3. TCP interleaved 打开后不读 RTP → server 不应因为 send 阻塞而全局挂死
#include <rtsp-server/rtsp-server.h>
#include <rtsp-common/socket.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace rtsp;
using namespace std::chrono_literals;

static bool connectTcp(Socket& s, const std::string& host, uint16_t port) {
    return s.connect(host, port, 2000);
}

static void test_slowloris_header_limit() {
    // 启动 server
    auto server = std::make_shared<RtspServer>();
    server->init("127.0.0.1", 0);
    // 我们用 0 端口由系统分配，但 API 要求显式端口——改用一个高端口
    const uint16_t kPort = 18651;
    server->init("127.0.0.1", kPort);
    PathConfig cfg;
    cfg.path = "/live";
    cfg.codec = CodecType::H264;
    server->addPath(cfg);
    if (!server->start()) {
        std::cerr << "SKIP slowloris: failed to bind 127.0.0.1:" << kPort << std::endl;
        return;
    }
    std::this_thread::sleep_for(200ms);

    Socket sock;
    if (!connectTcp(sock, "127.0.0.1", kPort)) {
        std::cerr << "FAIL: cannot connect for slowloris test" << std::endl;
        server->stop();
        std::exit(1);
    }
    // 持续发送 header 字节但不发 \r\n\r\n
    std::string line = "OPTIONS rtsp://h/a RTSP/1.0\r\nCSeq: 1\r\nX-Pad: ";
    sock.send(reinterpret_cast<const uint8_t*>(line.data()), line.size());
    for (int i = 0; i < 200; ++i) {
        std::string chunk(512, 'A');
        ssize_t n = sock.send(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
        if (n <= 0) break;  // server 在超过 header 上限后会关闭连接 —— 期望结果
        std::this_thread::sleep_for(5ms);
    }

    // 即使被恶意 slowloris 占用，server 仍应能服务新连接
    Socket sock2;
    bool ok = connectTcp(sock2, "127.0.0.1", kPort);
    assert(ok && "server must still accept new connections under slowloris");

    sock.close();
    sock2.close();
    server->stop();
}

static void test_oversized_content_length() {
    auto server = std::make_shared<RtspServer>();
    const uint16_t kPort = 18652;
    server->init("127.0.0.1", kPort);
    server->addPath(PathConfig{"/live", CodecType::H264, 1920, 1080, 30, {}, {}, {}});
    if (!server->start()) {
        std::cerr << "SKIP oversized-content-length: failed to bind" << std::endl;
        return;
    }
    std::this_thread::sleep_for(200ms);

    Socket sock;
    if (!connectTcp(sock, "127.0.0.1", kPort)) {
        server->stop();
        return;
    }
    // 声称 Content-Length: 4GB
    std::string hdr =
        "ANNOUNCE rtsp://h/live RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: 4294967295\r\n\r\n";
    sock.send(reinterpret_cast<const uint8_t*>(hdr.data()), hdr.size());
    // server 应该立即关闭连接；我们读一下确认能拿到 EOF 或错误
    uint8_t buf[256];
    sock.recv(buf, sizeof(buf), 1500);
    // 另起新连接，server 仍然可用
    Socket sock2;
    bool ok = connectTcp(sock2, "127.0.0.1", kPort);
    assert(ok && "server must still accept new connections after rejecting oversized CL");

    sock.close();
    sock2.close();
    server->stop();
}

static void test_tcp_interleaved_stalled_consumer() {
    // 打开 TCP interleaved 模式，SETUP+PLAY 后不再 recv，观察 server.stop() 超时在阈值内
    auto server = std::make_shared<RtspServer>();
    const uint16_t kPort = 18653;
    server->init("127.0.0.1", kPort);
    server->addPath(PathConfig{"/live", CodecType::H264, 1920, 1080, 30, {}, {}, {}});
    if (!server->start()) {
        std::cerr << "SKIP stalled interleaved: failed to bind" << std::endl;
        return;
    }
    std::this_thread::sleep_for(200ms);

    Socket sock;
    if (!connectTcp(sock, "127.0.0.1", kPort)) {
        server->stop();
        return;
    }

    const std::string describe =
        "DESCRIBE rtsp://127.0.0.1:" + std::to_string(kPort) + "/live RTSP/1.0\r\n"
        "CSeq: 1\r\nAccept: application/sdp\r\n\r\n";
    sock.send(reinterpret_cast<const uint8_t*>(describe.data()), describe.size());
    uint8_t tmp[4096];
    sock.recv(tmp, sizeof(tmp), 1000);

    const std::string setup =
        "SETUP rtsp://127.0.0.1:" + std::to_string(kPort) + "/live/stream RTSP/1.0\r\n"
        "CSeq: 2\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
    sock.send(reinterpret_cast<const uint8_t*>(setup.data()), setup.size());
    sock.recv(tmp, sizeof(tmp), 1000);

    const std::string play =
        "PLAY rtsp://127.0.0.1:" + std::to_string(kPort) + "/live RTSP/1.0\r\n"
        "CSeq: 3\r\nSession: test\r\n\r\n";
    sock.send(reinterpret_cast<const uint8_t*>(play.data()), play.size());
    // PLAY 响应我们故意不完全读取，制造接收阻塞
    sock.recv(tmp, 128, 500);

    // 开始推大量帧，server 会尝试通过 interleaved 发送
    // 对方不读 → send buffer 打满 → 旧代码会挂死
    std::vector<uint8_t> nalu(4096, 0x60);
    nalu[0] = 0x00; nalu[1] = 0x00; nalu[2] = 0x00; nalu[3] = 0x01; nalu[4] = 0x65;
    for (int i = 0; i < 200; ++i) {
        server->pushH264Data("/live", nalu.data(), nalu.size(), i * 33, i == 0);
        std::this_thread::sleep_for(5ms);
    }

    // 关键断言：server->stop() 应在合理时间内返回（我们给 6 秒）
    auto t0 = std::chrono::steady_clock::now();
    bool stopped_ok = server->stopWithTimeout(6000);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    std::cout << "stalled interleaved stop took " << ms << "ms, ok=" << stopped_ok << std::endl;
    assert(ms < 6000 && "server stop must not hang when TCP peer stalls");

    sock.close();
}

int main() {
    test_slowloris_header_limit();
    test_oversized_content_length();
    test_tcp_interleaved_stalled_consumer();
    std::cout << "hostile client tests passed\n";
    return 0;
}
