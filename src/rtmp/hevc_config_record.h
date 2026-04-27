#pragma once

// HEVCDecoderConfigurationRecord (ISO/IEC 14496-15 §8.3.3.1.2)，
// 用作 RTMP 推 H.265 时的 sequence header。
//
// 本实现生成的是"够用"版本：从 VPS/SPS/PPS 三个 NALU 里抽出基本字段，
// 剩余细节字段（如 profile_space / general_profile_idc 细分、
// general_constraint_indicator_flags 全 48 位）直接从 SPS 原样复制或填默认。
// 主流接收端（FFmpeg / mediamtx / SRS 新版）都能正常解析此最小实现。

#include <cstdint>
#include <vector>

namespace rtsp {

// vps/sps/pps：各自为"不含起始码的 NALU"字节序列。
// 至少需要 SPS；VPS/PPS 缺任一字段函数允许（会生成 numOfArrays=剩下的几个），
// 但现实里 H.265 的接收端通常三样都要。
std::vector<uint8_t> buildHevcDecoderConfigRecord(
    const std::vector<uint8_t>& vps,
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps);

}  // namespace rtsp
