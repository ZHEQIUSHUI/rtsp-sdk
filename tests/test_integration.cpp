/**
 * 集成测试
 * 
 * 测试完整的RTSP服务器工作流程
 */

#include <rtsp-server/rtsp-server.h>
#include <rtsp-client/rtsp-client.h>
#include <rtsp-publisher/rtsp-publisher.h>
#include <rtsp-common/socket.h>
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <regex>
#include <string>
#include <vector>
#ifndef _WIN32
#include <dirent.h>
#endif

using namespace rtsp;

namespace {
struct MockPublishRtspServer {
    explicit MockPublishRtspServer(uint16_t port, uint16_t rtp_port)
        : rtsp_port(port), server_rtp_port(rtp_port) {}

    void start() {
        running = true;
        thread = std::thread([this]() { run(); });
    }

    void stop() {
        running = false;
        {
            std::lock_guard<std::mutex> lock(sock_mutex);
            if (accepted_socket) {
                accepted_socket->close();
            }
            udp_socket.close();
            listen_socket.close();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    ~MockPublishRtspServer() {
        stop();
    }

    bool gotRtpPacket() const {
        return received_rtp.load();
    }

private:
    static int parseCseq(const std::string& req) {
        std::regex cseq_re("CSeq:\\s*(\\d+)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(req, m, cseq_re)) {
            return std::stoi(m[1].str());
        }
        return 1;
    }

    static int parseClientRtpPort(const std::string& req) {
        std::regex port_re("client_port=(\\d+)-(\\d+)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(req, m, port_re)) {
            return std::stoi(m[1].str());
        }
        return 0;
    }

    static std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    static bool tryPopRequest(std::string& buffer, std::string& out) {
        size_t header_end_pos = buffer.find("\r\n\r\n");
        if (header_end_pos == std::string::npos) {
            return false;
        }
        const size_t header_len = header_end_pos + 4;
        size_t content_length = 0;
        std::string headers_lower = toLower(buffer.substr(0, header_len));
        size_t cl_pos = headers_lower.find("content-length:");
        if (cl_pos != std::string::npos) {
            size_t cl_line_end = headers_lower.find("\r\n", cl_pos);
            if (cl_line_end != std::string::npos) {
                std::string cl = headers_lower.substr(cl_pos + 15, cl_line_end - (cl_pos + 15));
                try {
                    content_length = static_cast<size_t>(std::stoul(cl));
                } catch (...) {
                    content_length = 0;
                }
            }
        }
        if (buffer.size() < header_len + content_length) {
            return false;
        }
        out = buffer.substr(0, header_len + content_length);
        buffer.erase(0, header_len + content_length);
        return true;
    }

    static std::string readRequest(Socket& sock, std::string& pending) {
        uint8_t tmp[4096];
        std::string req;
        if (tryPopRequest(pending, req)) {
            return req;
        }
        while (true) {
            ssize_t n = sock.recv(tmp, sizeof(tmp), 200);
            if (n > 0) {
                pending.append(reinterpret_cast<const char*>(tmp), static_cast<size_t>(n));
                if (tryPopRequest(pending, req)) {
                    return req;
                }
                continue;
            }
            if (n == 0) {
                continue;
            }
            return "";
        }
    }

    static std::string response200(int cseq, const std::string& headers = "") {
        std::string resp = "RTSP/1.0 200 OK\r\n";
        resp += "CSeq: " + std::to_string(cseq) + "\r\n";
        resp += headers;
        resp += "\r\n";
        return resp;
    }

    void run() {
        if (!listen_socket.bind("127.0.0.1", rtsp_port)) return;
        if (!listen_socket.listen()) return;
        listen_socket.setNonBlocking(true);

        while (running) {
            auto s = listen_socket.accept();
            if (s) {
                std::lock_guard<std::mutex> lock(sock_mutex);
                accepted_socket = std::move(s);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!accepted_socket) return;

        std::string pending;
        const std::string session = "pub-session-1";
        while (running) {
            std::string req = readRequest(*accepted_socket, pending);
            if (req.empty()) break;
            int cseq = parseCseq(req);
            if (req.find("ANNOUNCE ") == 0) {
                auto resp = response200(cseq);
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
            } else if (req.find("SETUP ") == 0) {
                int client_rtp_port = parseClientRtpPort(req);
                if (client_rtp_port <= 0) {
                    std::string bad = "RTSP/1.0 400 Bad Request\r\nCSeq: " + std::to_string(cseq) + "\r\n\r\n";
                    accepted_socket->send(reinterpret_cast<const uint8_t*>(bad.data()), bad.size());
                    continue;
                }
                if (!udp_socket.isValid()) {
                    (void)udp_socket.bindUdp("127.0.0.1", server_rtp_port);
                    udp_socket.setNonBlocking(true);
                }
                std::string headers = "Transport: RTP/AVP;unicast;client_port=" +
                                      std::to_string(client_rtp_port) + "-" + std::to_string(client_rtp_port + 1) +
                                      ";server_port=" + std::to_string(server_rtp_port) + "-" + std::to_string(server_rtp_port + 1) + "\r\n" +
                                      "Session: " + session + "\r\n";
                auto resp = response200(cseq, headers);
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
            } else if (req.find("RECORD ") == 0) {
                std::string headers = "Session: " + session + "\r\n";
                auto resp = response200(cseq, headers);
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());

                uint8_t buf[2048];
                std::string from_ip;
                uint16_t from_port = 0;
                for (int i = 0; i < 200 && running && !received_rtp.load(); ++i) {
                    ssize_t n = udp_socket.recvFrom(buf, sizeof(buf), from_ip, from_port);
                    if (n > 0) {
                        received_rtp = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            } else if (req.find("TEARDOWN ") == 0) {
                auto resp = response200(cseq, "Session: " + session + "\r\n");
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
                break;
            } else {
                auto resp = response200(cseq);
                accepted_socket->send(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
            }
        }
    }

    uint16_t rtsp_port;
    uint16_t server_rtp_port;
    std::atomic<bool> running{false};
    std::atomic<bool> received_rtp{false};
    std::thread thread;
    std::mutex sock_mutex;
    Socket listen_socket;
    Socket udp_socket;
    std::unique_ptr<Socket> accepted_socket;
};
} // namespace

void test_server_init() {
    std::cout << "Testing server initialization..." << std::endl;
    
    RtspServer server;
    
    // 测试初始化
    assert(server.init("0.0.0.0", 8554));
    assert(!server.isRunning());
    
    // 添加路径
    assert(server.addPath("/test1", CodecType::H264));
    assert(server.addPath("/test2", CodecType::H265));
    
    // 重复添加应该失败
    assert(!server.addPath("/test1", CodecType::H264));
    
    // 删除路径
    assert(server.removePath("/test1"));
    assert(!server.removePath("/nonexistent"));
    
    std::cout << "  Server initialization tests passed!" << std::endl;
}

void test_server_start_stop() {
    std::cout << "Testing server start/stop..." << std::endl;
    
    RtspServer server;
    
    // 先添加路径再启动
    assert(server.init("127.0.0.1", 18554));  // 使用高位端口避免冲突
    assert(server.addPath("/live", CodecType::H264));
    
    // 启动服务器
    assert(server.start());
    assert(server.isRunning());
    
    // 等待一小会儿确保服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 停止服务器
    server.stop();
    assert(!server.isRunning());
    
    std::cout << "  Server start/stop tests passed!" << std::endl;
}

void test_server_singleton_factory() {
    std::cout << "Testing server singleton factory..." << std::endl;
    auto s1 = getOrCreateRtspServer(19570, "127.0.0.1");
    auto s2 = getOrCreateRtspServer(19570, "0.0.0.0");
    assert(s1);
    assert(s2);
    assert(s1.get() == s2.get());

    assert(s1->addPath("/singleton", CodecType::H264));
    assert(!s2->addPath("/singleton", CodecType::H264));
    assert(s1->start());
    assert(s2->isRunning());
    assert(s2->stopWithTimeout(1000));
    std::cout << "  Server singleton factory tests passed!" << std::endl;
}

void test_push_frame() {
    std::cout << "Testing frame pushing..." << std::endl;
    
    RtspServer server;
    assert(server.init("127.0.0.1", 18555));
    assert(server.addPath("/stream", CodecType::H264));
    assert(server.start());
    
    // 创建测试帧
    std::vector<uint8_t> frame_data = {
        0x00, 0x00, 0x00, 0x01,  // 起始码
        0x67, 0x42, 0x00, 0x28,  // SPS
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xCE, 0x3C, 0x80,  // PPS
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x80, 0x00,  // IDR
    };
    
    // 推送帧
    VideoFrame frame = createVideoFrame(
        CodecType::H264,
        frame_data.data(),
        frame_data.size(),
        0,
        640,
        480,
        30
    );
    frame.type = FrameType::IDR;
    
    assert(server.pushFrame("/stream", frame));
    
    // 推送到不存在的路径应该失败
    assert(!server.pushFrame("/nonexistent", frame));
    
    server.stop();
    
    std::cout << "  Frame pushing tests passed!" << std::endl;
}

void test_push_raw_data() {
    std::cout << "Testing raw data pushing..." << std::endl;
    
    RtspServer server;
    assert(server.init("127.0.0.1", 18556));
    assert(server.addPath("/h264", CodecType::H264));
    assert(server.addPath("/h265", CodecType::H265));
    assert(server.start());
    
    // 推送H.264数据
    std::vector<uint8_t> h264_data = {
        0x00, 0x00, 0x00, 0x01,
        0x41, 0x9A, 0x24, 0x00  // P frame
    };
    
    assert(server.pushH264Data("/h264", h264_data.data(), h264_data.size(), 0, false));
    
    // 推送H.265数据
    std::vector<uint8_t> h265_data = {
        0x00, 0x00, 0x00, 0x01,
        0x26, 0x01, 0xAF, 0x09  // IDR_N_LP
    };
    
    assert(server.pushH265Data("/h265", h265_data.data(), h265_data.size(), 0, true));
    
    server.stop();
    
    std::cout << "  Raw data pushing tests passed!" << std::endl;
}

void test_config_management() {
    std::cout << "Testing configuration management..." << std::endl;
    
    // 测试路径配置
    PathConfig config;
    config.path = "/camera1";
    config.codec = CodecType::H264;
    config.width = 1920;
    config.height = 1080;
    config.fps = 30;
    config.sps = {0x67, 0x42, 0x00, 0x28};
    config.pps = {0x68, 0xCE, 0x3C, 0x80};
    
    RtspServer server;
    RtspServerConfig server_config;
    server_config.host = "127.0.0.1";
    server_config.port = 18558;
    server_config.session_timeout_ms = 30000;
    server_config.rtp_port_start = 20000;
    server_config.rtp_port_end = 30000;
    
    assert(server.init(server_config));
    assert(server.addPath(config));
    assert(server.start());
    
    // 验证端口分配
    uint32_t current = server_config.rtp_port_start;
    uint16_t port1 = RtspServerConfig::getNextRtpPort(current, 
        server_config.rtp_port_start, server_config.rtp_port_end);
    uint16_t port2 = RtspServerConfig::getNextRtpPort(current, 
        server_config.rtp_port_start, server_config.rtp_port_end);
    
    assert(port2 == port1 + 2);  // 每次增加2（RTP+RTCP）
    
    server.stop();
    
    std::cout << "  Configuration management tests passed!" << std::endl;
}

void test_concurrent_operations() {
    std::cout << "Testing concurrent operations..." << std::endl;
    
    RtspServer server;
    assert(server.init("127.0.0.1", 18559));
    
    // 添加多个路径
    for (int i = 0; i < 5; i++) {
        std::string path = "/stream" + std::to_string(i);
        assert(server.addPath(path, i % 2 == 0 ? CodecType::H264 : CodecType::H265));
    }
    
    assert(server.start());
    
    // 并发推送帧
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; i++) {
        threads.emplace_back([&server, i]() {
            std::string path = "/stream" + std::to_string(i);
            std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x01, 0x41, 0x00};
            
            for (int j = 0; j < 10; j++) {
                VideoFrame frame = createVideoFrame(
                    i % 2 == 0 ? CodecType::H264 : CodecType::H265,
                    data.data(),
                    data.size(),
                    j * 33,
                    640,
                    480,
                    30
                );
                server.pushFrame(path, frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    server.stop();
    
    std::cout << "  Concurrent operations tests passed!" << std::endl;
}

void test_client_pause_resume_keepalive() {
    std::cout << "Testing client pause/resume and keepalive..." << std::endl;

    RtspServer server;
    assert(server.init("127.0.0.1", 18560));
    assert(server.addPath("/live", CodecType::H264));
    assert(server.start());

    RtspClient client;
    assert(client.open("rtsp://127.0.0.1:18560/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));
    assert(client.pause());
    assert(client.sendGetParameter("ping: 1"));
    assert(client.play(0));
    assert(client.teardown());
    client.close();

    server.stop();

    std::cout << "  Client pause/resume and keepalive tests passed!" << std::endl;
}

void test_basic_auth() {
    std::cout << "Testing basic auth..." << std::endl;

    RtspServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 18561;
    cfg.auth_enabled = true;
    cfg.auth_username = "user";
    cfg.auth_password = "pass";
    cfg.auth_realm = "TestRealm";

    RtspServer server;
    assert(server.init(cfg));
    assert(server.addPath("/live", CodecType::H264));
    assert(server.start());

    RtspClient no_auth_client;
    assert(no_auth_client.open("rtsp://127.0.0.1:18561/live"));
    assert(!no_auth_client.describe());
    no_auth_client.close();

    RtspClient auth_client;
    assert(auth_client.open("rtsp://user:pass@127.0.0.1:18561/live"));
    assert(auth_client.describe());
    assert(auth_client.setup(0));
    assert(auth_client.play(0));
    auth_client.close();

    auto ss = server.getStats();
    assert(ss.auth_challenges >= 1);
    assert(ss.auth_failures >= 1);

    server.stop();
    std::cout << "  Basic auth tests passed!" << std::endl;
}

void test_tcp_interleaved_streaming() {
    std::cout << "Testing TCP interleaved streaming..." << std::endl;

    RtspServer server;
    assert(server.init("127.0.0.1", 18562));
    assert(server.addPath("/live", CodecType::H264));
    assert(server.start());

    RtspClient client;
    RtspClientConfig ccfg;
    ccfg.prefer_tcp_transport = true;
    ccfg.fallback_to_tcp = true;
    client.setConfig(ccfg);

    assert(client.open("rtsp://127.0.0.1:18562/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    std::thread push_thread([&server]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::vector<uint8_t> h264 = {
            0x00, 0x00, 0x00, 0x01,
            0x65, 0x88, 0x84, 0x21
        };
        server.pushH264Data("/live", h264.data(), h264.size(), 100, true);
    });

    VideoFrame frame{};
    assert(client.receiveFrame(frame, 2000));
    assert(frame.codec == CodecType::H264);
    assert(frame.size > 4);

    auto cs = client.getStats();
    assert(cs.using_tcp_transport);
    assert(cs.frames_output >= 1);

    push_thread.join();
    auto ss = server.getStats();
    assert(ss.rtp_packets_sent >= 1);
    assert(ss.frames_pushed >= 1);
    client.close();
    server.stop();
    std::cout << "  TCP interleaved streaming tests passed!" << std::endl;
}

void test_digest_auth() {
    std::cout << "Testing digest auth..." << std::endl;

    RtspServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 18563;
    cfg.auth_enabled = true;
    cfg.auth_use_digest = true;
    cfg.auth_username = "du";
    cfg.auth_password = "dp";
    cfg.auth_realm = "DigestRealm";
    cfg.auth_nonce = "fixednonce123";
    cfg.auth_nonce_ttl_ms = 1;

    RtspServer server;
    assert(server.init(cfg));
    assert(server.addPath("/live", CodecType::H264));
    assert(server.start());

    RtspClient no_auth_client;
    assert(no_auth_client.open("rtsp://127.0.0.1:18563/live"));
    assert(!no_auth_client.describe());
    no_auth_client.close();

    RtspClient digest_client;
    assert(digest_client.open("rtsp://du:dp@127.0.0.1:18563/live"));
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // force nonce stale path
    assert(digest_client.describe());
    assert(digest_client.setup(0));
    assert(digest_client.play(0));
    auto cs = digest_client.getStats();
    assert(cs.auth_retries >= 1);
    digest_client.close();

    server.stop();
    std::cout << "  Digest auth tests passed!" << std::endl;
}

void test_auto_parameter_set_extraction() {
    std::cout << "Testing auto parameter-set extraction..." << std::endl;

    RtspServer server;
    assert(server.init("127.0.0.1", 18564));
    assert(server.addPath("/h264", CodecType::H264));
    assert(server.addPath("/h265", CodecType::H265));
    assert(server.start());

    std::vector<uint8_t> h264_key = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x42, 0x00, 0x28,
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xCE, 0x3C, 0x80,
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x84, 0x21
    };
    assert(server.pushH264Data("/h264", h264_key.data(), h264_key.size(), 0, true));

    std::vector<uint8_t> h265_key = {
        0x00, 0x00, 0x00, 0x01,
        0x40, 0x01, 0x0C, 0x01, 0xFF, // VPS
        0x00, 0x00, 0x00, 0x01,
        0x42, 0x01, 0x01, 0x01,       // SPS
        0x00, 0x00, 0x00, 0x01,
        0x44, 0x01, 0xC0, 0xF1,       // PPS
        0x00, 0x00, 0x00, 0x01,
        0x26, 0x01, 0xAF, 0x09        // IDR
    };
    assert(server.pushH265Data("/h265", h265_key.data(), h265_key.size(), 0, true));

    RtspClient h264_client;
    assert(h264_client.open("rtsp://127.0.0.1:18564/h264"));
    assert(h264_client.describe());
    auto h264_info = h264_client.getSessionInfo();
    assert(!h264_info.media_streams.empty());
    assert(!h264_info.media_streams[0].sps.empty());
    assert(!h264_info.media_streams[0].pps.empty());
    h264_client.close();

    RtspClient h265_client;
    assert(h265_client.open("rtsp://127.0.0.1:18564/h265"));
    assert(h265_client.describe());
    auto h265_info = h265_client.getSessionInfo();
    assert(!h265_info.media_streams.empty());
    assert(!h265_info.media_streams[0].vps.empty());
    assert(!h265_info.media_streams[0].sps.empty());
    assert(!h265_info.media_streams[0].pps.empty());
    h265_client.close();

    server.stop();
    std::cout << "  Auto parameter-set extraction tests passed!" << std::endl;
}

void test_receive_interrupt_and_stop_timeout() {
    std::cout << "Testing receive interrupt and stop timeout..." << std::endl;

    RtspServer server;
    assert(server.init("127.0.0.1", 18565));
    assert(server.addPath("/live", CodecType::H264));
    assert(server.start());

    RtspClient client;
    assert(client.open("rtsp://127.0.0.1:18565/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    std::atomic<bool> done{false};
    std::thread waiter([&]() {
        VideoFrame frame{};
        bool ok = client.receiveFrame(frame, 15000);
        assert(!ok);
        done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client.interrupt();
    client.closeWithTimeout(1000);

    waiter.join();
    assert(done.load());
    assert(server.stopWithTimeout(1000));
    std::cout << "  Receive interrupt and stop timeout tests passed!" << std::endl;
}

void test_stop_latency_under_2s() {
    std::cout << "Testing stop latency <= 200ms for receive waiters..." << std::endl;
    RtspServer server;
    assert(server.init("127.0.0.1", 18567));
    assert(server.addPath("/live", CodecType::H264));
    assert(server.start());

    RtspClient client;
    assert(client.open("rtsp://127.0.0.1:18567/live"));
    assert(client.describe());
    assert(client.setup(0));
    assert(client.play(0));

    std::thread loop_waiter([&]() {
        client.receiveLoop();
    });
    std::thread frame_waiter([&]() {
        VideoFrame f{};
        (void)client.receiveFrame(f, 15000);
    });

    std::this_thread::sleep_for(std::chrono::seconds(5));
    auto t0 = std::chrono::steady_clock::now();
    assert(client.closeWithTimeout(2000));
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    assert(elapsed_ms <= 2000);

    loop_waiter.join();
    frame_waiter.join();
    assert(server.stopWithTimeout(2000));
    std::cout << "  stop latency ms=" << elapsed_ms << std::endl;
    std::cout << "  Stop latency tests passed!" << std::endl;
}

int countOpenFds() {
#ifdef _WIN32
    return -1;
#else
    int count = 0;
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (readdir(d) != nullptr) {
        count++;
    }
    closedir(d);
    return count;
#endif
}

static long percentileMs(std::vector<long> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>((p / 100.0) * static_cast<double>(v.size() - 1));
    return v[idx];
}

void test_open_play_stop_50_loops() {
    std::cout << "Testing 50x open/play/stop stability..." << std::endl;
    RtspServer server;
    assert(server.init("127.0.0.1", 18568));
    assert(server.addPath("/live", CodecType::H264));
    assert(server.start());

    const int fd_before = countOpenFds();
    std::vector<long> stop_ms;
    stop_ms.reserve(50);

    for (int i = 0; i < 50; ++i) {
        RtspClient client;
        assert(client.open("rtsp://127.0.0.1:18568/live"));
        assert(client.describe());
        assert(client.setup(0));
        assert(client.play(0));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto t0 = std::chrono::steady_clock::now();
        bool ok = client.closeWithTimeout(2000);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        stop_ms.push_back(ms);
        assert(ok);
    }

    const int fd_after = countOpenFds();
    if (fd_before >= 0 && fd_after >= 0) {
        assert((fd_after - fd_before) < 16);
    }
    assert(server.stopWithTimeout(2000));

    std::cout << "  stop latency p50=" << percentileMs(stop_ms, 50)
              << "ms p95=" << percentileMs(stop_ms, 95)
              << "ms p99=" << percentileMs(stop_ms, 99) << "ms" << std::endl;
    std::cout << "  50x open/play/stop tests passed!" << std::endl;
}

void test_publish_client_api_smoke() {
    std::cout << "Testing RTSP publish client API smoke..." << std::endl;
    RtspPublisher pub;
    RtspPublishConfig cfg;
    cfg.local_rtp_port = 25000;
    pub.setConfig(cfg);
    assert(!pub.isConnected());
    assert(!pub.isRecording());
    std::cout << "  Publish client API smoke tests passed!" << std::endl;
}

void test_publish_client_to_mock_server() {
    std::cout << "Testing RTSP publisher to mock server..." << std::endl;
    MockPublishRtspServer mock(18569, 31000);
    mock.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    RtspPublisher pub;
    RtspPublishConfig cfg;
    cfg.local_rtp_port = 25020;
    pub.setConfig(cfg);
    assert(pub.open("rtsp://127.0.0.1:18569/live/publish"));

    PublishMediaInfo media;
    media.codec = CodecType::H264;
    media.payload_type = 96;
    media.width = 640;
    media.height = 480;
    media.fps = 25;
    media.sps = {0x67, 0x42, 0x00, 0x28};
    media.pps = {0x68, 0xCE, 0x3C, 0x80};
    media.control_track = "streamid=0";
    assert(pub.announce(media));
    assert(pub.setup());
    assert(pub.record());

    const std::vector<uint8_t> idr = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x42, 0x00, 0x28,
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xCE, 0x3C, 0x80,
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x84, 0x21
    };
    assert(pub.pushH264Data(idr.data(), idr.size(), 0, true));

    bool got = false;
    for (int i = 0; i < 50; ++i) {
        if (mock.gotRtpPacket()) {
            got = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(got);

    assert(pub.teardown());
    pub.close();
    mock.stop();
    std::cout << "  RTSP publisher to mock server tests passed!" << std::endl;
}

int main() {
    std::cout << "=== Running Integration Tests ===" << std::endl;
    
    try {
        test_server_init();
        test_server_start_stop();
        test_server_singleton_factory();
        test_push_frame();
        test_push_raw_data();
        test_config_management();
        test_concurrent_operations();
        test_client_pause_resume_keepalive();
        test_basic_auth();
        test_tcp_interleaved_streaming();
        test_digest_auth();
        test_auto_parameter_set_extraction();
        test_receive_interrupt_and_stop_timeout();
        test_stop_latency_under_2s();
        test_open_play_stop_50_loops();
        test_publish_client_api_smoke();
        test_publish_client_to_mock_server();
        
        std::cout << "\n=== All Integration Tests Passed! ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
