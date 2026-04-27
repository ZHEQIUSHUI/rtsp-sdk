#include "avc_config_record.h"

namespace rtsp {

std::vector<uint8_t> buildAvcDecoderConfigRecord(
    const uint8_t* sps, size_t sps_len,
    const uint8_t* pps, size_t pps_len) {

    std::vector<uint8_t> out;
    if (!sps || sps_len < 4 || !pps || pps_len == 0) return out;

    out.reserve(11 + sps_len + pps_len);
    out.push_back(1);                     // configurationVersion
    out.push_back(sps[1]);                // AVCProfileIndication (profile_idc)
    out.push_back(sps[2]);                // profile_compatibility
    out.push_back(sps[3]);                // AVCLevelIndication (level_idc)
    out.push_back(0xFF);                  // lengthSizeMinusOne = 3 (i.e. 4 bytes length)
    out.push_back(0xE1);                  // 1 SPS (高 3 位 reserved 全 1)
    out.push_back(static_cast<uint8_t>((sps_len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(sps_len & 0xFF));
    out.insert(out.end(), sps, sps + sps_len);
    out.push_back(1);                     // 1 PPS
    out.push_back(static_cast<uint8_t>((pps_len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(pps_len & 0xFF));
    out.insert(out.end(), pps, pps + pps_len);
    return out;
}

std::vector<uint8_t> buildAvcDecoderConfigRecord(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps) {
    return buildAvcDecoderConfigRecord(sps.data(), sps.size(), pps.data(), pps.size());
}

}  // namespace rtsp
