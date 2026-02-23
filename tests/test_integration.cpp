/**
 * 集成测试
 * 
 * 测试完整的RTSP服务器工作流程
 */

#include <rtsp-server/rtsp-server.h>
#include <rtsp-client/rtsp-client.h>
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

using namespace rtsp;

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
    delete[] frame.data;

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

int main() {
    std::cout << "=== Running Integration Tests ===" << std::endl;
    
    try {
        test_server_init();
        test_server_start_stop();
        test_push_frame();
        test_push_raw_data();
        test_config_management();
        test_concurrent_operations();
        test_client_pause_resume_keepalive();
        test_basic_auth();
        test_tcp_interleaved_streaming();
        test_digest_auth();
        test_auto_parameter_set_extraction();
        
        std::cout << "\n=== All Integration Tests Passed! ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
