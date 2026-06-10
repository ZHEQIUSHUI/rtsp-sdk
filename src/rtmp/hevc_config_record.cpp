#include "hevc_config_record.h"

#include <cstring>

namespace rtsp {

namespace {

void appendNaluArray(std::vector<uint8_t>& out,
                     uint8_t nal_unit_type,
                     const std::vector<uint8_t>& nalu) {
    if (nalu.empty()) return;
    // array_completeness(1) | reserved(1)=0 | NAL_unit_type(6)
    out.push_back(static_cast<uint8_t>(0x80 | (nal_unit_type & 0x3F)));
    // numNalus (u16 BE) = 1
    out.push_back(0x00); out.push_back(0x01);
    // nalUnitLength (u16 BE)
    out.push_back(static_cast<uint8_t>((nalu.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(nalu.size() & 0xFF));
    // bytes
    out.insert(out.end(), nalu.begin(), nalu.end());
}

// 去除 NAL 里的 emulation-prevention 字节（00 00 03 -> 00 00），得到 RBSP。
// profile_tier_level 必须在 RBSP 上读：Main/Main10 的 compatibility+constraint
// 区几乎全是 00，编码器必然插入 00 00 03，裸拷原始 NAL 字节会整体错位。
std::vector<uint8_t> stripEmulationPrevention(const uint8_t* p, size_t n) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        // 紧跟两个 0x00 之后的 0x03 是 emulation_prevention_three_byte，丢弃
        if (i >= 2 && p[i] == 0x03 && p[i - 1] == 0x00 && p[i - 2] == 0x00) {
            continue;
        }
        rbsp.push_back(p[i]);
    }
    return rbsp;
}

}  // namespace

// H.265 NAL unit types（选用到的）
constexpr uint8_t kHevcNalVps = 32;
constexpr uint8_t kHevcNalSps = 33;
constexpr uint8_t kHevcNalPps = 34;

std::vector<uint8_t> buildHevcDecoderConfigRecord(
    const std::vector<uint8_t>& vps,
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps) {

    std::vector<uint8_t> out;

    // profile_tier_level 位于 SPS 的 RBSP 中，先去除 emulation-prevention 字节再读。
    // RBSP 布局：[0..1] NAL 头, [2] sps_video_parameter_set_id(4)|
    //            sps_max_sub_layers_minus1(3)|sps_temporal_id_nesting(1),
    //            [3] general_profile_space(2)|tier_flag(1)|profile_idc(5),
    //            [4..7] general_profile_compatibility_flags(32),
    //            [8..13] general_constraint_indicator_flags(48),
    //            [14] general_level_idc(8)
    std::vector<uint8_t> rbsp = stripEmulationPrevention(sps.data(), sps.size());
    if (rbsp.size() < 15) {
        // SPS 太短，无法安全提取 profile_tier_level；返回空让调用方放弃
        return out;
    }
    const uint8_t* s = rbsp.data();

    // ---- 22-byte fixed header ----
    out.push_back(0x01);                // configurationVersion = 1

    // general_profile_space(2)|tier_flag(1)|profile_idc(5)
    out.push_back(s[3]);

    // general_profile_compatibility_flags (32 bits)
    out.push_back(s[4]);
    out.push_back(s[5]);
    out.push_back(s[6]);
    out.push_back(s[7]);

    // general_constraint_indicator_flags (48 bits)
    out.push_back(s[8]);
    out.push_back(s[9]);
    out.push_back(s[10]);
    out.push_back(s[11]);
    out.push_back(s[12]);
    out.push_back(s[13]);

    // general_level_idc (8 bits)
    out.push_back(s[14]);

    // reserved(4)|min_spatial_segmentation_idc(12) = 0xF000
    out.push_back(0xF0); out.push_back(0x00);
    // reserved(6)|parallelismType(2) = 0xFC (parallelism unknown)
    out.push_back(0xFC);
    // reserved(6)|chromaFormat(2) = 0xFD（默认 4:2:0）
    out.push_back(0xFD);
    // reserved(5)|bitDepthLumaMinus8(3) = 0xF8
    out.push_back(0xF8);
    // reserved(5)|bitDepthChromaMinus8(3) = 0xF8
    out.push_back(0xF8);
    // avgFrameRate (u16 BE) = 0
    out.push_back(0x00); out.push_back(0x00);
    // constantFrameRate(2)|numTemporalLayers(3)|temporalIdNested(1)|lengthSizeMinusOne(2)
    // = 0 | 1 | 1 | 3 = 0x0F
    out.push_back(0x0F);

    // ---- numOfArrays (u8) + 每个 array ----
    uint8_t num_arrays = 0;
    if (!vps.empty()) ++num_arrays;
    if (!sps.empty()) ++num_arrays;
    if (!pps.empty()) ++num_arrays;
    out.push_back(num_arrays);

    appendNaluArray(out, kHevcNalVps, vps);
    appendNaluArray(out, kHevcNalSps, sps);
    appendNaluArray(out, kHevcNalPps, pps);

    return out;
}

}  // namespace rtsp
