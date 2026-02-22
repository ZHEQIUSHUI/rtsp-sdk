/**
 * RTP打包测试
 */

#include <rtsp-server/rtsp-server.h>
#include "rtp_packer.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace rtsp;

// 创建H.264 IDR NALU
std::vector<uint8_t> createH264IdrNalu(size_t size) {
    std::vector<uint8_t> data;
    data.reserve(size + 4);
    
    // 起始码
    data.insert(data.end(), {0x00, 0x00, 0x00, 0x01});
    
    // NALU头 (IDR)
    data.push_back(0x65);  // NAL type 5 (IDR)
    
    // 填充数据
    for (size_t i = 0; i < size - 5; i++) {
        data.push_back(i & 0xFF);
    }
    
    return data;
}

// 创建H.264 P帧NALU
std::vector<uint8_t> createH264PNalu(size_t size) {
    std::vector<uint8_t> data;
    data.reserve(size + 4);
    
    // 起始码
    data.insert(data.end(), {0x00, 0x00, 0x00, 0x01});
    
    // NALU头 (P frame)
    data.push_back(0x41);  // NAL type 1 (non-IDR)
    
    // 填充数据
    for (size_t i = 0; i < size - 5; i++) {
        data.push_back(i & 0xFF);
    }
    
    return data;
}

void test_h264_single_nalu() {
    std::cout << "Testing H.264 single NALU packing..." << std::endl;
    
    // 小包（直接打包）
    auto nalu_data = createH264IdrNalu(100);
    
    VideoFrame frame = createVideoFrame(
        CodecType::H264,
        nalu_data.data(),
        nalu_data.size(),
        1000,
        1920,
        1080,
        30
    );
    frame.type = FrameType::IDR;
    
    H264RtpPacker packer;
    auto packets = packer.packFrame(frame);
    
    // 小包应该只生成一个RTP包
    assert(packets.size() == 1);
    
    const auto& packet = packets[0];
    
    // 验证RTP头
    assert(packet.size == 12 + nalu_data.size() - 4);  // RTP头 + NALU（去掉起始码）
    assert((packet.data[0] & 0xC0) == 0x80);  // V=2
    assert(packet.data[1] == 96);  // payload type
    assert(packet.marker);  // 单包应该有marker
    
    // 验证序列号
    assert(packet.seq == 0);
    
    // 验证时间戳
    uint32_t expected_ts = convertToRtpTimestamp(1000, 90000);
    assert(packet.timestamp == expected_ts);
    
    // 清理
    for (auto& p : packets) {
        delete[] p.data;
    }
    
    std::cout << "  H.264 single NALU tests passed!" << std::endl;
}

void test_h264_fragmentation() {
    std::cout << "Testing H.264 FU-A fragmentation..." << std::endl;
    
    // 创建一个大NALU（需要分片）
    auto nalu_data = createH264IdrNalu(3000);
    
    VideoFrame frame = createVideoFrame(
        CodecType::H264,
        nalu_data.data(),
        nalu_data.size(),
        2000,
        1920,
        1080,
        30
    );
    
    H264RtpPacker packer;
    packer.setMtu(1500);  // 设置MTU
    
    auto packets = packer.packFrame(frame);
    
    // 大包应该生成多个RTP包
    assert(packets.size() > 1);
    
    // 验证每个包
    for (size_t i = 0; i < packets.size(); i++) {
        const auto& packet = packets[i];
        
        // 验证RTP头
        assert(packet.size >= 14);  // RTP头 + FU指示器 + FU头 + 数据
        assert((packet.data[0] & 0xC0) == 0x80);  // V=2
        
        // 验证FU指示器 (28 = FU-A)
        assert((packet.data[12] & 0x1F) == 28);
        
        // 验证FU头
        uint8_t fu_header = packet.data[13];
        if (i == 0) {
            // 第一个包应该有S位
            assert(fu_header & 0x80);
            assert(!(fu_header & 0x40));  // 不应该有E位
        } else if (i == packets.size() - 1) {
            // 最后一个包应该有E位
            assert(fu_header & 0x40);
            assert(!(fu_header & 0x80));  // 不应该有S位
            assert(packet.marker);  // 最后一个包应该有marker
        } else {
            // 中间包不应该有S或E位
            assert(!(fu_header & 0x80));
            assert(!(fu_header & 0x40));
        }
        
        delete[] packet.data;
    }
    
    std::cout << "  H.264 FU-A fragmentation tests passed!" << std::endl;
}

void test_h265_packing() {
    std::cout << "Testing H.265 packing..." << std::endl;
    
    // 创建H.265 NALU
    std::vector<uint8_t> nalu_data;
    nalu_data.insert(nalu_data.end(), {0x00, 0x00, 0x00, 0x01});  // 起始码
    nalu_data.insert(nalu_data.end(), {0x26, 0x01});  // NALU头 (IDR_N_LP)
    
    // 填充数据
    for (int i = 0; i < 100; i++) {
        nalu_data.push_back(i & 0xFF);
    }
    
    VideoFrame frame = createVideoFrame(
        CodecType::H265,
        nalu_data.data(),
        nalu_data.size(),
        3000,
        1920,
        1080,
        30
    );
    
    H265RtpPacker packer;
    auto packets = packer.packFrame(frame);
    
    // 小包应该只生成一个RTP包
    assert(packets.size() == 1);
    
    const auto& packet = packets[0];
    
    // 验证RTP头
    assert((packet.data[0] & 0xC0) == 0x80);  // V=2
    assert(packet.data[1] == 96);  // payload type
    
    // 验证H.265 payload header
    uint8_t payload_type = (packet.data[12] >> 1) & 0x3F;
    assert(payload_type == 0x13);  // IDR_N_LP = 19 (0x13)
    
    delete[] packet.data;
    
    std::cout << "  H.265 packing tests passed!" << std::endl;
}

void test_h265_fragmentation() {
    std::cout << "Testing H.265 fragmentation..." << std::endl;
    
    // 创建大H.265 NALU
    std::vector<uint8_t> nalu_data;
    nalu_data.insert(nalu_data.end(), {0x00, 0x00, 0x00, 0x01});  // 起始码
    nalu_data.insert(nalu_data.end(), {0x26, 0x01});  // NALU头 (IDR_N_LP)
    
    // 填充大量数据
    for (int i = 0; i < 5000; i++) {
        nalu_data.push_back(i & 0xFF);
    }
    
    VideoFrame frame = createVideoFrame(
        CodecType::H265,
        nalu_data.data(),
        nalu_data.size(),
        4000,
        1920,
        1080,
        30
    );
    
    H265RtpPacker packer;
    packer.setMtu(1500);
    
    auto packets = packer.packFrame(frame);
    
    // 大包应该生成多个RTP包
    assert(packets.size() > 1);
    
    // 验证每个包
    for (size_t i = 0; i < packets.size(); i++) {
        const auto& packet = packets[i];
        
        // H.265 FU包头是3字节
        assert(packet.size >= 15);  // RTP头 + FU头(3字节) + 数据
        
        // 验证FU类型 (49 = FU)
        uint8_t fu_type = (packet.data[12] >> 1) & 0x3F;
        assert(fu_type == 49);
        
        // 验证FU头
        uint8_t fu_header = packet.data[14];
        if (i == 0) {
            assert(fu_header & 0x80);  // S位
        } else if (i == packets.size() - 1) {
            assert(fu_header & 0x40);  // E位
            assert(packet.marker);
        }
        
        delete[] packet.data;
    }
    
    std::cout << "  H.265 fragmentation tests passed!" << std::endl;
}

void test_sequence_number() {
    std::cout << "Testing sequence numbers..." << std::endl;
    
    H264RtpPacker packer;
    
    // 打包多个帧
    for (int frame_num = 0; frame_num < 3; frame_num++) {
        auto nalu_data = createH264PNalu(100);
        VideoFrame frame = createVideoFrame(
            CodecType::H264,
            nalu_data.data(),
            nalu_data.size(),
            frame_num * 33,
            640,
            480,
            30
        );
        
        auto packets = packer.packFrame(frame);
        
        // 验证序列号递增
        for (size_t i = 0; i < packets.size(); i++) {
            assert(packets[i].seq == frame_num);  // 每帧一个包
            delete[] packets[i].data;
        }
    }
    
    std::cout << "  Sequence number tests passed!" << std::endl;
}

int main() {
    std::cout << "=== Running RTP Tests ===" << std::endl;
    
    try {
        test_h264_single_nalu();
        test_h264_fragmentation();
        test_h265_packing();
        test_h265_fragmentation();
        test_sequence_number();
        
        std::cout << "\n=== All RTP Tests Passed! ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
