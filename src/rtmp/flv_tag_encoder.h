#pragma once

// FLV Video Tag payload 构造器（RTMP message type 9 的 body 格式）。
//
// H.264（标准 FLV）：
//   byte0: frameType(4) | codecID=7(4)
//          frameType: 1=key, 2=inter
//   byte1: AVCPacketType: 0=seq header, 1=NALU, 2=end
//   byte2..4: CompositionTime offset (signed int24 BE, PTS-DTS in ms)
//   body  : seq=AVCDecoderConfigurationRecord 或 NALU=[4B length|NALU]+
//
// H.265 国内兼容（codecID=12）：与 H.264 结构一致，只是 codecID 填 12（0x0C）。
// 国内 CDN / 播放器（B 站 / 抖音 / 快手）大多兼容这种"非标准"扩展。
//
// H.265 Enhanced RTMP（FourCC=hvc1）：
//   byte0: IsExHeader(1)|PacketType(4)|reserved(3?)
//          实际规范：byte0 高位 bit=1 表示 "isExHeader"，
//          低 4 位 = PacketType（0=SeqStart, 1=CodedFrames, 2=SeqEnd, 3=CodedFramesX）
//          中间 3 位为 frameType（1=key, 2=inter, 3=disposable, ...）。
//          字节值示例：0x9C（ex=1, frameType=1, packetType=CodedFrames? 见实现）
//   byte1..4: FourCC "hvc1"
//   若 PacketType=1(CodedFrames): 3B CompositionTime offset，然后 NALU 数据
//   若 PacketType=3(CodedFramesX): 无 CompositionTime 字段（即 dts=pts）
//   若 PacketType=0(SeqStart):     HEVCDecoderConfigurationRecord
//
// 本实现统一：
//   - H.264 → legacy 格式
//   - H.265 mode=0 → Enhanced RTMP（推荐）
//   - H.265 mode=1 → legacy codecID=12

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtsp {

// 把 Annex-B 字节流转成 AVCC 形式（每个 NALU 前 4 字节大端长度）。
// data/len 允许包含多个 NALU，起始码可以是 3 字节或 4 字节。
std::vector<uint8_t> annexBToAvcc(const uint8_t* data, size_t len);

// ============== H.264 ==============
// video tag = seq header（AVCDecoderConfigurationRecord）
std::vector<uint8_t> buildFlvVideoTagH264SeqHeader(
    const std::vector<uint8_t>& avc_config_record);

// video tag = NALU frame
std::vector<uint8_t> buildFlvVideoTagH264Frame(
    const std::vector<uint8_t>& avcc_payload,
    bool is_key,
    int32_t composition_time_ms);

// ============== H.265: legacy (codecID=12, China-compatible) ==============
std::vector<uint8_t> buildFlvVideoTagH265LegacySeqHeader(
    const std::vector<uint8_t>& hvc_config_record);

std::vector<uint8_t> buildFlvVideoTagH265LegacyFrame(
    const std::vector<uint8_t>& avcc_payload,
    bool is_key,
    int32_t composition_time_ms);

// ============== H.265: Enhanced RTMP (FourCC="hvc1") ==============
std::vector<uint8_t> buildFlvVideoTagH265EnhancedSeqHeader(
    const std::vector<uint8_t>& hvc_config_record);

std::vector<uint8_t> buildFlvVideoTagH265EnhancedFrame(
    const std::vector<uint8_t>& avcc_payload,
    bool is_key,
    int32_t composition_time_ms);

}  // namespace rtsp
