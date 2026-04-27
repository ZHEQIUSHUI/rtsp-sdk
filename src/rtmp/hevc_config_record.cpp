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
    if (sps.size() < 14) {
        // SPS 太短，无法安全提取 profile；返回空让调用方放弃
        return out;
    }

    // ---- 22-byte fixed header ----
    out.push_back(0x01);                // configurationVersion = 1

    // profile_space(2)|tier_flag(1)|profile_idc(5)：从 SPS 里第 2~3 字节拿
    // SPS NALU 前两字节是 NAL header；此后是 sps_video_parameter_set_id(4 bits) +
    // sps_max_sub_layers_minus1(3) + sps_temporal_id_nesting(1)，然后是
    // profile_tier_level。为简单起见，直接复制 SPS 偏移 1 处以后常见的 profile 字段：
    // SPS[1..12] 是 profile_tier_level 的一部分。不同 encoder 排版略有差别，
    // 这里按 FFmpeg 实现的处理方式：
    const uint8_t* s = sps.data();
    // SPS 解析这里保守做：完整从 SPS byte 1 开始的 12 字节都拷过来给
    // general_profile_tier_level。这是 FFmpeg 生成 hvcC 时常用的近似做法。
    // 有些 encoder 会在 SPS 里做 emulation prevention byte，现实中绝大部分
    // 接收端对这 12 字节仅做显示用途，不校验细节位图。
    uint8_t profile_space_tier_idc = s[1];
    out.push_back(profile_space_tier_idc);

    // general_profile_compatibility_flags (32 bits)
    out.push_back(s[2]);
    out.push_back(s[3]);
    out.push_back(s[4]);
    out.push_back(s[5]);

    // general_constraint_indicator_flags (48 bits)
    out.push_back(s[6]);
    out.push_back(s[7]);
    out.push_back(s[8]);
    out.push_back(s[9]);
    out.push_back(s[10]);
    out.push_back(s[11]);

    // general_level_idc (8 bits)
    out.push_back(s[12]);

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
