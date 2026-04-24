#include "flv_tag_encoder.h"

#include <cstring>

namespace rtsp {

namespace {

void writeBE24s(std::vector<uint8_t>& out, int32_t v) {
    // int24 signed big-endian。RTMP 里 CompositionTime 一般为 0 或很小正数。
    const uint32_t u = static_cast<uint32_t>(v) & 0x00FFFFFFu;
    out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 8)  & 0xFF));
    out.push_back(static_cast<uint8_t>( u        & 0xFF));
}

void writeBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8)  & 0xFF));
    out.push_back(static_cast<uint8_t>( v        & 0xFF));
}

bool isStart3(const uint8_t* p, size_t rem) {
    return rem >= 3 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01;
}
bool isStart4(const uint8_t* p, size_t rem) {
    return rem >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x01;
}

}  // namespace

std::vector<uint8_t> annexBToAvcc(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    if (!data || len == 0) return out;
    out.reserve(len + 8);

    // 逐个 NALU 提取（按起始码分割），每段 4B 长度 + NALU 字节。
    // 以便后续接收端按 lengthSizeMinusOne=3 正确拆分。
    size_t i = 0;
    while (i < len) {
        // 跳过当前位置的起始码
        size_t sc = 0;
        if (isStart4(data + i, len - i)) sc = 4;
        else if (isStart3(data + i, len - i)) sc = 3;

        if (sc > 0) {
            i += sc;
        } else if (out.empty()) {
            // 整段都没起始码（某些 encoder 的裸 NALU 输入）
            writeBE32(out, static_cast<uint32_t>(len));
            out.insert(out.end(), data, data + len);
            return out;
        } else {
            // 边界错乱：直接返回已解析部分
            break;
        }

        // 寻找下一个起始码，作为当前 NALU 的结束
        size_t j = i;
        while (j < len) {
            if (isStart3(data + j, len - j) || isStart4(data + j, len - j)) break;
            ++j;
        }
        const size_t nalu_len = j - i;
        if (nalu_len == 0) continue;

        writeBE32(out, static_cast<uint32_t>(nalu_len));
        out.insert(out.end(), data + i, data + i + nalu_len);
        i = j;
    }
    return out;
}

// ===================== H.264 =====================

std::vector<uint8_t> buildFlvVideoTagH264SeqHeader(
    const std::vector<uint8_t>& avc_config_record) {
    std::vector<uint8_t> out;
    out.reserve(5 + avc_config_record.size());
    // frameType=1 (key) | codecID=7
    out.push_back(0x17);
    // AVCPacketType=0 (seq header)
    out.push_back(0x00);
    // CompositionTime = 0
    writeBE24s(out, 0);
    out.insert(out.end(), avc_config_record.begin(), avc_config_record.end());
    return out;
}

std::vector<uint8_t> buildFlvVideoTagH264Frame(
    const std::vector<uint8_t>& avcc_payload,
    bool is_key,
    int32_t composition_time_ms) {
    std::vector<uint8_t> out;
    out.reserve(5 + avcc_payload.size());
    // frameType | codecID=7
    out.push_back(static_cast<uint8_t>((is_key ? 1 : 2) << 4 | 0x07));
    // AVCPacketType=1 (NALU)
    out.push_back(0x01);
    writeBE24s(out, composition_time_ms);
    out.insert(out.end(), avcc_payload.begin(), avcc_payload.end());
    return out;
}

// ===================== H.265 legacy (codecID=12) =====================

std::vector<uint8_t> buildFlvVideoTagH265LegacySeqHeader(
    const std::vector<uint8_t>& hvc_config_record) {
    std::vector<uint8_t> out;
    out.reserve(5 + hvc_config_record.size());
    // frameType=1 | codecID=12
    out.push_back(0x1C);
    out.push_back(0x00);           // PacketType=0 seq header
    writeBE24s(out, 0);
    out.insert(out.end(), hvc_config_record.begin(), hvc_config_record.end());
    return out;
}

std::vector<uint8_t> buildFlvVideoTagH265LegacyFrame(
    const std::vector<uint8_t>& avcc_payload,
    bool is_key,
    int32_t composition_time_ms) {
    std::vector<uint8_t> out;
    out.reserve(5 + avcc_payload.size());
    out.push_back(static_cast<uint8_t>((is_key ? 1 : 2) << 4 | 0x0C));
    out.push_back(0x01);
    writeBE24s(out, composition_time_ms);
    out.insert(out.end(), avcc_payload.begin(), avcc_payload.end());
    return out;
}

// ===================== H.265 Enhanced RTMP =====================
//
// Enhanced RTMP video header 字节 0 的编码：
//   bit7   : isExHeader = 1
//   bit6..4: frameType (1=key, 2=inter, 3=disposable, 4=generated-key, 5=reserved)
//   bit3..0: PacketType (0=SeqStart, 1=CodedFrames, 2=SeqEnd, 3=CodedFramesX)

namespace {

constexpr uint8_t kExHdrBit = 0x80;

uint8_t buildExByte0(bool is_key, uint8_t packet_type) {
    const uint8_t frame_type = is_key ? 1 : 2;
    return static_cast<uint8_t>(kExHdrBit | ((frame_type & 0x07) << 4) | (packet_type & 0x0F));
}

}  // namespace

std::vector<uint8_t> buildFlvVideoTagH265EnhancedSeqHeader(
    const std::vector<uint8_t>& hvc_config_record) {
    std::vector<uint8_t> out;
    out.reserve(5 + hvc_config_record.size());
    // SeqStart 固定 frameType=1
    out.push_back(buildExByte0(true, 0));
    // FourCC "hvc1"
    out.push_back('h'); out.push_back('v'); out.push_back('c'); out.push_back('1');
    out.insert(out.end(), hvc_config_record.begin(), hvc_config_record.end());
    return out;
}

std::vector<uint8_t> buildFlvVideoTagH265EnhancedFrame(
    const std::vector<uint8_t>& avcc_payload,
    bool is_key,
    int32_t composition_time_ms) {
    std::vector<uint8_t> out;
    out.reserve(8 + avcc_payload.size());
    // PacketType=1 (CodedFrames，带 CompositionTime)
    out.push_back(buildExByte0(is_key, 1));
    out.push_back('h'); out.push_back('v'); out.push_back('c'); out.push_back('1');
    writeBE24s(out, composition_time_ms);
    out.insert(out.end(), avcc_payload.begin(), avcc_payload.end());
    return out;
}

}  // namespace rtsp
