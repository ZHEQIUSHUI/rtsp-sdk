// DoS 加固回归测试。
//
// 这些上限之前是手工验证的：把它们固化成 ctest，防止后续重构悄悄放开对
// "不可信对端 payload" 的防护，导致栈溢出 / 内存被钉死。全部是纯函数、
// 无网络、跨平台，是最稳定的一类测试。
//
// 覆盖：
//   1. AMF0 解析嵌套深度上限（parseValue depth cap）——恶意 RTMP 服务器
//      可发超深嵌套 StrictArray 撑爆解析栈。
//   2. RTMP chunk 单消息长度上限（msg_len ≤ 16MB 由对端控制）——超限拒绝。
//   3. RTMP chunk 并发 csid 数上限——防 cs_states_ map 被无限撑大。

#include "amf0_codec.h"
#include "rtmp_chunk_stream.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace rtsp;

namespace {

// 追加一个 AMF0 number（marker 0x00 + 8 字节 double，这里内容无所谓）
void appendAmf0Number(std::vector<uint8_t>& b) {
    b.push_back(0x00);
    for (int i = 0; i < 8; ++i) b.push_back(0x00);
}

// 追加一层 StrictArray 头（marker 0x0A + u32 count=1）。嵌套 N 层后跟一个终端值。
void appendStrictArrayLevel(std::vector<uint8_t>& b) {
    b.push_back(0x0a);
    b.push_back(0x00); b.push_back(0x00); b.push_back(0x00); b.push_back(0x01);  // count = 1
}

std::vector<uint8_t> makeNestedStrictArray(int levels) {
    std::vector<uint8_t> b;
    for (int i = 0; i < levels; ++i) appendStrictArrayLevel(b);
    appendAmf0Number(b);  // 最内层放个 number 收尾
    return b;
}

void put_be24(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((v >> 16) & 0xFF);
    b.push_back((v >> 8) & 0xFF);
    b.push_back(v & 0xFF);
}

// 追加一个 Format 0 chunk（完整消息头）。csid 支持 1 字节(2..63)/2 字节(64..319) basic header。
// payload 必须 ≤ 默认 in_chunk_size(128)，否则会被切成多 chunk（本测试不需要）。
void appendFmt0Chunk(std::vector<uint8_t>& b, uint32_t csid, uint8_t type_id,
                     const std::vector<uint8_t>& payload) {
    if (csid >= 2 && csid <= 63) {
        b.push_back(static_cast<uint8_t>((0 << 6) | csid));
    } else {
        b.push_back(0x00);                                    // fmt=0, csid marker 0
        b.push_back(static_cast<uint8_t>(csid - 64));
    }
    put_be24(b, 0);                                           // timestamp
    put_be24(b, static_cast<uint32_t>(payload.size()));       // msg length
    b.push_back(type_id);                                     // type id
    b.push_back(0x01); b.push_back(0); b.push_back(0); b.push_back(0);  // stream id (LE) = 1
    b.insert(b.end(), payload.begin(), payload.end());
}

// ---- 测试 1：AMF0 嵌套深度 ----
void test_amf0_nesting_depth() {
    // 合法的浅层嵌套（5 层）应能解析成功（返回消费字节数 > 0），证明上限只拒绝过深，
    // 不是一刀切拒绝所有嵌套。
    {
        auto data = makeNestedStrictArray(5);
        amf0::Value v;
        size_t n = amf0::parseValue(data.data(), data.size(), &v);
        assert(n == data.size() && "shallow nested StrictArray should parse fully");
        assert(v.type == amf0::Type::StrictArray);
    }
    // 超深嵌套（64 层 > 32 上限）必须被拒绝（返回 0），且不崩溃 / 不栈溢出。
    {
        auto data = makeNestedStrictArray(64);
        amf0::Value v;
        size_t n = amf0::parseValue(data.data(), data.size(), &v);
        assert(n == 0 && "over-deep nested AMF0 must be rejected (returns 0)");
    }
    std::cout << "[ok] amf0 nesting depth cap\n";
}

// ---- 测试 2：RTMP 单消息长度上限 ----
void test_rtmp_message_length_cap() {
    // 正常小消息：feed 应返回 true 并产出一条完整消息，payload 正确。
    {
        ChunkStreamDecoder dec;
        std::vector<uint8_t> payload = {0x17, 0x00, 0x01, 0x02, 0x03};  // 5 字节
        std::vector<uint8_t> wire;
        appendFmt0Chunk(wire, rtmp_csid::kVideo, rtmp_msg::kVideo, payload);
        std::vector<RtmpMessage> msgs;
        bool ok = dec.feed(wire.data(), wire.size(), &msgs);
        assert(ok && "valid small chunk must be accepted");
        assert(msgs.size() == 1);
        assert(msgs[0].type_id == rtmp_msg::kVideo);
        assert(msgs[0].payload == payload);
    }
    // 恶意：msg_len = 0xFFFFFF（≈16 MiB，24bit 字段最大值，> 8 MiB 上限）。手工拼一个
    // fmt0 头，长度字段填最大，feed 必须返回 false（协议错误 → 调用方断连），不会尝试
    // 分配/累积 16MB。注意 0x1000000 会溢出 24bit 回绕成 0，所以用 0xFFFFFF。
    {
        ChunkStreamDecoder dec;
        std::vector<uint8_t> wire;
        wire.push_back(static_cast<uint8_t>((0 << 6) | rtmp_csid::kVideo));  // fmt0, csid5
        put_be24(wire, 0);              // timestamp
        put_be24(wire, 0xFFFFFF);       // msg length = 16 MiB-1 > 8 MiB cap
        wire.push_back(rtmp_msg::kVideo);
        wire.push_back(0x01); wire.push_back(0); wire.push_back(0); wire.push_back(0);
        // 后面故意不给 payload：cap 检查在切片之前，应直接判错
        std::vector<RtmpMessage> msgs;
        bool ok = dec.feed(wire.data(), wire.size(), &msgs);
        assert(!ok && "oversized RTMP msg_len must be rejected");
    }
    std::cout << "[ok] rtmp message length cap\n";
}

// ---- 测试 3：RTMP 并发 csid 数上限 ----
void test_rtmp_csid_cap() {
    // 上限是 64。喂 64 个不同 csid 的完整小消息应全部接受；第 65 个新 csid 触发拒绝。
    ChunkStreamDecoder dec;
    std::vector<uint8_t> payload = {0xAA};
    // 已知合法 csid 从 64 开始（2 字节 basic header），连续 65 个：64..128
    bool saw_reject = false;
    for (uint32_t i = 0; i < 65; ++i) {
        std::vector<uint8_t> wire;
        appendFmt0Chunk(wire, 64 + i, rtmp_msg::kAudio, payload);
        std::vector<RtmpMessage> msgs;
        bool ok = dec.feed(wire.data(), wire.size(), &msgs);
        if (!ok) { saw_reject = true; break; }
    }
    assert(saw_reject && "exceeding max concurrent csids must be rejected");
    std::cout << "[ok] rtmp csid cap\n";
}

}  // namespace

int main() {
    test_amf0_nesting_depth();
    test_rtmp_message_length_cap();
    test_rtmp_csid_cap();
    std::cout << "all dos-hardening tests passed\n";
    return 0;
}
