// AVCDecoderConfigurationRecord / HEVCDecoderConfigurationRecord 以及
// Annex-B→AVCC / FLV video tag 字节级正确性
#include "avc_config_record.h"
#include "hevc_config_record.h"
#include "flv_tag_encoder.h"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace rtsp;

static void test_avc_decoder_config_record() {
    // 最小可用 SPS（4 字节足以做 profile/level 提取）+ PPS
    const std::vector<uint8_t> sps = {0x67, 0x42, 0xC0, 0x1F, 0xD9, 0x00, 0x78};
    const std::vector<uint8_t> pps = {0x68, 0xCE, 0x3C, 0x80};
    const auto r = buildAvcDecoderConfigRecord(sps, pps);
    assert(!r.empty());
    assert(r[0] == 1);               // configurationVersion
    assert(r[1] == sps[1]);           // profile_idc
    assert(r[2] == sps[2]);           // profile_compatibility
    assert(r[3] == sps[3]);           // level_idc
    assert(r[4] == 0xFF);             // lengthSizeMinusOne=3
    assert(r[5] == 0xE1);             // numOfSPS=1
    // sps length = 7
    assert(r[6] == 0 && r[7] == 7);
    assert(std::memcmp(r.data() + 8, sps.data(), sps.size()) == 0);
    // numOfPPS
    size_t off = 8 + sps.size();
    assert(r[off++] == 1);
    assert(r[off++] == 0 && r[off++] == 4);  // pps length = 4
    assert(std::memcmp(r.data() + off, pps.data(), pps.size()) == 0);
}

static void test_annexb_to_avcc_multi_nalu() {
    // 两个 NALU，起始码混用 4 字节和 3 字节
    const std::vector<uint8_t> in = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F,   // SPS
        0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,          // PPS (3-byte SC)
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x00,    // IDR
    };
    const auto out = annexBToAvcc(in.data(), in.size());
    // 每个 NALU 前 4 字节长度；共三段
    // 长度字段：4 (SPS), 4 (PPS), 4 (IDR) —— 实际 NALU 长度是 4/4/4
    // 总字节：12 长度字段 + 12 NALU = 24
    assert(out.size() == 24);
    // 第一段长度=4
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 4);
    // NALU 内容
    assert(out[4] == 0x67 && out[5] == 0x42 && out[6] == 0xC0 && out[7] == 0x1F);
}

static void test_h264_video_tag_key() {
    const std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x04, 0x65, 0x88, 0x80, 0x00};
    const auto tag = buildFlvVideoTagH264Frame(avcc, /*is_key=*/true, 0);
    assert(tag.size() == 5 + avcc.size());
    assert(tag[0] == 0x17);     // keyframe + AVC
    assert(tag[1] == 0x01);     // AVCPacketType=1 (NALU)
    assert(tag[2] == 0 && tag[3] == 0 && tag[4] == 0);  // CompositionTime=0
    assert(std::memcmp(tag.data() + 5, avcc.data(), avcc.size()) == 0);
}

static void test_h264_video_tag_inter() {
    const std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x04, 0x41, 0x01, 0x02, 0x03};
    const auto tag = buildFlvVideoTagH264Frame(avcc, /*is_key=*/false, 0);
    assert(tag[0] == 0x27);     // inter frame + AVC
    assert(tag[1] == 0x01);
}

static void test_h265_legacy_seq_header() {
    const std::vector<uint8_t> fake_hvc = {0xAA, 0xBB, 0xCC};
    const auto tag = buildFlvVideoTagH265LegacySeqHeader(fake_hvc);
    assert(tag[0] == 0x1C);     // keyframe + codecID=12
    assert(tag[1] == 0x00);     // seq header
    assert(tag[5] == 0xAA);
}

static void test_h265_enhanced_seq_header() {
    const std::vector<uint8_t> fake_hvc = {0xAA};
    const auto tag = buildFlvVideoTagH265EnhancedSeqHeader(fake_hvc);
    // byte0: 0x80 | frameType=1<<4 | packetType=0 = 0x90
    assert(tag[0] == 0x90);
    // FourCC
    assert(tag[1] == 'h' && tag[2] == 'v' && tag[3] == 'c' && tag[4] == '1');
    assert(tag[5] == 0xAA);
}

static void test_h265_enhanced_frame_with_cts() {
    const std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x02, 0x11, 0x22};
    const auto tag = buildFlvVideoTagH265EnhancedFrame(avcc, /*is_key=*/true, 10);
    // byte0 = 0x80 | 1<<4 | 1 = 0x91
    assert(tag[0] == 0x91);
    // FourCC
    assert(tag[1] == 'h' && tag[2] == 'v' && tag[3] == 'c' && tag[4] == '1');
    // CompositionTime = 10 (3 bytes BE)
    assert(tag[5] == 0 && tag[6] == 0 && tag[7] == 10);
    // payload
    assert(tag[8] == 0x00 && tag[9] == 0x00 && tag[10] == 0x00 && tag[11] == 0x02);
}

static void test_hvc_config_basic_smoke() {
    // 造一个足够长的 SPS 使 buildHevcDecoderConfigRecord 不返回空（只看 smoke）
    std::vector<uint8_t> vps(10, 0x55);
    std::vector<uint8_t> sps(32, 0x66);
    std::vector<uint8_t> pps(5,  0x77);
    const auto r = buildHevcDecoderConfigRecord(vps, sps, pps);
    assert(!r.empty());
    assert(r[0] == 1);         // configurationVersion
    assert(r[22] == 3);        // numOfArrays = 3
}

int main() {
    test_avc_decoder_config_record();
    test_annexb_to_avcc_multi_nalu();
    test_h264_video_tag_key();
    test_h264_video_tag_inter();
    test_h265_legacy_seq_header();
    test_h265_enhanced_seq_header();
    test_h265_enhanced_frame_with_cts();
    test_hvc_config_basic_smoke();
    std::cout << "rtmp flv tests passed\n";
    return 0;
}
