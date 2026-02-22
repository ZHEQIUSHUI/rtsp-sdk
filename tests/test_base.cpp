/**
 * 基础功能测试
 */

#include <rtsp-server/rtsp-server.h>
#include "rtp_packer.h"
#include "sdp.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace rtsp;

void test_base64() {
    std::cout << "Testing Base64..." << std::endl;
    
    // 测试编码
    std::vector<uint8_t> data = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::string encoded = base64Encode(data.data(), data.size());
    assert(encoded == "AAECAwQF");
    
    // 测试解码
    auto decoded = base64Decode(encoded);
    assert(decoded == data);
    
    // 测试空数据
    std::string empty_encoded = base64Encode(nullptr, 0);
    assert(empty_encoded.empty());
    
    std::cout << "  Base64 tests passed!" << std::endl;
}

void test_video_frame() {
    std::cout << "Testing VideoFrame..." << std::endl;
    
    // 创建测试数据
    uint8_t test_data[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x28};
    
    VideoFrame frame = createVideoFrame(
        CodecType::H264,
        test_data,
        sizeof(test_data),
        1000,
        1920,
        1080,
        30
    );
    
    assert(frame.codec == CodecType::H264);
    assert(frame.size == sizeof(test_data));
    assert(frame.pts == 1000);
    assert(frame.width == 1920);
    assert(frame.height == 1080);
    assert(frame.fps == 30);
    
    std::cout << "  VideoFrame tests passed!" << std::endl;
}

void test_rtp_timestamp() {
    std::cout << "Testing RTP timestamp conversion..." << std::endl;
    
    // H.264/H.265通常使用90000Hz时钟
    uint32_t ts1 = convertToRtpTimestamp(0, 90000);
    assert(ts1 == 0);
    
    uint32_t ts2 = convertToRtpTimestamp(1000, 90000);  // 1秒
    assert(ts2 == 90000);
    
    uint32_t ts3 = convertToRtpTimestamp(100, 90000);   // 100ms
    assert(ts3 == 9000);
    
    std::cout << "  RTP timestamp tests passed!" << std::endl;
}

void test_nalu_parsing() {
    std::cout << "Testing NALU parsing..." << std::endl;
    
    // 构建测试NALU数据
    std::vector<uint8_t> data;
    // NALU 1: SPS
    data.insert(data.end(), {0x00, 0x00, 0x00, 0x01});  // 起始码
    data.insert(data.end(), {0x67, 0x42, 0x00, 0x28});  // SPS NALU
    // NALU 2: PPS
    data.insert(data.end(), {0x00, 0x00, 0x01});        // 短起始码
    data.insert(data.end(), {0x68, 0xCE, 0x3C, 0x80});  // PPS NALU
    // NALU 3: IDR
    data.insert(data.end(), {0x00, 0x00, 0x00, 0x01});  // 起始码
    data.insert(data.end(), {0x65, 0x88, 0x80, 0x00});  // IDR NALU
    
    // H.264打包器会解析NALU
    H264RtpPacker packer;
    VideoFrame frame = createVideoFrame(
        CodecType::H264,
        data.data(),
        data.size(),
        0,
        640,
        480,
        30
    );
    
    auto packets = packer.packFrame(frame);
    
    // 应该生成3个RTP包（每个NALU一个）
    assert(packets.size() == 3);
    
    // 验证RTP头
    for (const auto& packet : packets) {
        assert(packet.size >= 12);  // 最小RTP头大小
        assert((packet.data[0] & 0xC0) == 0x80);  // V=2
        
        // 清理
        delete[] packet.data;
    }
    
    std::cout << "  NALU parsing tests passed!" << std::endl;
}

void test_sdp_builder() {
    std::cout << "Testing SDP builder..." << std::endl;
    
    SdpBuilder builder;
    builder.setVersion(0);
    builder.setOrigin("-", 1234567890, 0, "IN", "IP4", "127.0.0.1");
    builder.setSessionName("Test Stream");
    builder.setConnection("IN", "IP4", "0.0.0.0");
    builder.setTime(0, 0);
    
    std::string sps_b64 = "Z0LgKdpA";
    std::string pps_b64 = "aM4MgA==";
    
    builder.addH264Media("stream", 0, 96, 90000, sps_b64, pps_b64, 1920, 1080);
    
    std::string sdp = builder.build();
    
    // 验证SDP包含关键字段
    assert(sdp.find("v=0") != std::string::npos);
    assert(sdp.find("m=video") != std::string::npos);
    assert(sdp.find("H264/90000") != std::string::npos);
    assert(sdp.find("sprop-parameter-sets") != std::string::npos);
    assert(sdp.find("a=control:stream") != std::string::npos);
    
    std::cout << "  SDP builder tests passed!" << std::endl;
}

int main() {
    std::cout << "=== Running Base Tests ===" << std::endl;
    
    try {
        test_base64();
        test_video_frame();
        test_rtp_timestamp();
        test_nalu_parsing();
        test_sdp_builder();
        
        std::cout << "\n=== All Base Tests Passed! ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
