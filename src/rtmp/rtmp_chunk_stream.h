#pragma once

// RTMP Chunk Stream 编解码（RTMP spec §6）。
//
// 发送侧：给一条 "message"（csid + type + stream id + timestamp + payload），
// 自动分片成若干 chunk 写出去：
//   - 首个 chunk 用 Format 0（完整消息头，11 字节）
//   - 连续 chunk 用 Format 3（只有 Basic Header）
//   - timestamp >= 0xFFFFFF 时带 4 字节 Extended Timestamp
//
// 接收侧：喂原始字节，按 csid 维护"上次消息头"供 Format 1/2/3 继承；
// 一条消息（可能跨多个 chunk）装满后返回给调用方。

#include <rtsp-common/socket.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {

// RTMP 消息类型 id（type_id），用常量而不是 enum，方便少量打日志
namespace rtmp_msg {
constexpr uint8_t kSetChunkSize       = 1;
constexpr uint8_t kAbort              = 2;
constexpr uint8_t kAcknowledgement    = 3;
constexpr uint8_t kUserControl        = 4;
constexpr uint8_t kWindowAckSize      = 5;
constexpr uint8_t kSetPeerBandwidth   = 6;
constexpr uint8_t kAudio              = 8;
constexpr uint8_t kVideo              = 9;
constexpr uint8_t kDataAmf3           = 15;
constexpr uint8_t kSharedObjAmf3      = 16;
constexpr uint8_t kCommandAmf3        = 17;
constexpr uint8_t kDataAmf0           = 18;
constexpr uint8_t kSharedObjAmf0      = 19;
constexpr uint8_t kCommandAmf0        = 20;
constexpr uint8_t kAggregate          = 22;
}

// RTMP chunk stream id（csid）习惯分配：
namespace rtmp_csid {
constexpr uint32_t kProtocolControl = 2;  // chunk size / ack / ping
constexpr uint32_t kInvoke          = 3;  // AMF command
constexpr uint32_t kVideo           = 5;  // video data
constexpr uint32_t kAudio           = 6;
constexpr uint32_t kData            = 4;  // @setDataFrame / onMetaData
}

struct RtmpMessage {
    uint32_t csid          = 0;
    uint8_t  type_id       = 0;
    uint32_t msg_stream_id = 0;
    uint32_t timestamp     = 0;   // 绝对时间戳（收发侧在这一层都用绝对值对外呈现）
    std::vector<uint8_t> payload;
};

class ChunkStreamEncoder {
public:
    ChunkStreamEncoder();

    // 默认输出 chunk 大小 128（spec 默认）。publisher 通常在 connect 后提升。
    void setOutChunkSize(uint32_t size);
    uint32_t outChunkSize() const { return out_chunk_size_; }

    // 写一条消息到 socket。内部串行切分，必要时插入 Extended Timestamp。
    // 返回 true 表示所有字节已在 send_timeout_ms 内写出。
    bool writeMessage(Socket& s, const RtmpMessage& msg, int send_timeout_ms);

    // 计数器（方便调试 / 统计）
    uint64_t chunksOut() const { return chunks_out_; }
    uint64_t bytesOut()  const { return bytes_out_; }

private:
    uint32_t out_chunk_size_ = 128;
    uint64_t chunks_out_ = 0;
    uint64_t bytes_out_  = 0;
};

class ChunkStreamDecoder {
public:
    ChunkStreamDecoder();

    void setInChunkSize(uint32_t size);
    uint32_t inChunkSize() const { return in_chunk_size_; }

    // 投喂字节。把解码到的完整消息追加到 out_messages。
    // 返回 false 表示遇到了不可恢复的协议错误（调用方应断开连接）。
    bool feed(const uint8_t* data, size_t len, std::vector<RtmpMessage>* out_messages);

    uint64_t bytesIn()  const { return bytes_in_; }
    uint64_t messagesIn() const { return messages_in_; }

private:
    struct CsState {
        // 最近一次 Format 0/1/2 记录的元信息，后续 Format 1/2/3 继承
        uint32_t timestamp     = 0;
        uint32_t timestamp_delta = 0;
        uint32_t msg_length    = 0;
        uint8_t  msg_type_id   = 0;
        uint32_t msg_stream_id = 0;
        // 部分已到达的消息 payload
        std::vector<uint8_t> partial;
        bool has_last_fmt0_or_1 = false;  // 用来决定 Format 3 是否补加 extended timestamp
        bool last_ts_was_delta = false;
    };

    uint32_t in_chunk_size_ = 128;
    std::map<uint32_t, CsState> cs_states_;
    // 粘包缓冲
    std::vector<uint8_t> buffer_;
    uint64_t bytes_in_ = 0;
    uint64_t messages_in_ = 0;

    // 单条 chunk 解析：成功返回消费字节数；0 表示数据不完整（等更多）；
    // -1 表示协议错误
    int parseOneChunk(const uint8_t* data, size_t len,
                      std::vector<RtmpMessage>* out_messages);
};

}  // namespace rtsp
