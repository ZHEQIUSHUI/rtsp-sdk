#pragma once

// AVCDecoderConfigurationRecord（ISO/IEC 14496-15 §5.2.4.1），RTMP 的 H.264
// sequence header payload（在 FLV video tag 里 AVCPacketType=0 那条）。
//
// 字节布局：
//   configurationVersion (u8)  = 1
//   AVCProfileIndication (u8)  = SPS[1]
//   profile_compatibility (u8) = SPS[2]
//   AVCLevelIndication   (u8)  = SPS[3]
//   reserved(6)|lengthSizeMinusOne(2) = 0xFF (lengthSize=4)
//   reserved(3)|numOfSPS(5)           = 0xE1 (1 SPS)
//   SPS length (u16 BE) | SPS bytes
//   numOfPPS (u8)
//   PPS length (u16 BE) | PPS bytes

#include <cstdint>
#include <vector>

namespace rtsp {

std::vector<uint8_t> buildAvcDecoderConfigRecord(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps);

// 也提供一个按传入 SPS/PPS 的指针+长度版本，调用方无需构造 vector
std::vector<uint8_t> buildAvcDecoderConfigRecord(
    const uint8_t* sps, size_t sps_len,
    const uint8_t* pps, size_t pps_len);

}  // namespace rtsp
