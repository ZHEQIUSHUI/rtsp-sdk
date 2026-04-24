#include "rtmp_chunk_stream.h"

#include <rtsp-common/common.h>

#include <algorithm>
#include <cstring>

namespace rtsp {

namespace {

void writeBE24(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8)  & 0xFF);
    p[2] = static_cast<uint8_t>( v        & 0xFF);
}
void writeBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8)  & 0xFF);
    p[3] = static_cast<uint8_t>( v        & 0xFF);
}
// message stream id 是 little-endian（RTMP spec 特殊规定，与其他字段大端相反）
void writeLE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>( v        & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8)  & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t readBE24(const uint8_t* p) {
    return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
}
uint32_t readBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}
uint32_t readLE32(const uint8_t* p) {
    return  uint32_t(p[0])        | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace

// =============================== Encoder ===============================

ChunkStreamEncoder::ChunkStreamEncoder() = default;

void ChunkStreamEncoder::setOutChunkSize(uint32_t size) {
    if (size < 1) size = 1;
    if (size > 0x7FFFFFFF) size = 0x7FFFFFFF;
    out_chunk_size_ = size;
}

bool ChunkStreamEncoder::writeMessage(Socket& s, const RtmpMessage& msg, int send_timeout_ms) {
    if (!s.isValid()) return false;

    // 1. Basic Header：这里 publisher 永远用 1 字节形式（csid 2..63 全覆盖业务需求）
    //    如果将来 csid 超出 63 再扩展为 2/3 字节。
    if (msg.csid < 2 || msg.csid > 63) {
        RTSP_LOG_ERROR("ChunkStreamEncoder: csid " + std::to_string(msg.csid) +
                       " out of 1-byte range");
        return false;
    }

    // Extended Timestamp 判定
    const bool ext_ts = msg.timestamp >= 0xFFFFFF;
    const uint32_t ts_field = ext_ts ? 0xFFFFFF : msg.timestamp;

    // 2. 构造并发送第一个 chunk：Format 0（完整消息头）
    //    Basic(1) + MsgHeader(11) + [ExtTs(4)] + data(min(payload, chunk_size))
    uint8_t hdr[1 + 11 + 4];
    size_t hdr_len = 0;
    hdr[hdr_len++] = static_cast<uint8_t>((0 << 6) | (msg.csid & 0x3F));  // fmt=0
    writeBE24(hdr + hdr_len, ts_field); hdr_len += 3;
    writeBE24(hdr + hdr_len, static_cast<uint32_t>(msg.payload.size())); hdr_len += 3;
    hdr[hdr_len++] = msg.type_id;
    writeLE32(hdr + hdr_len, msg.msg_stream_id); hdr_len += 4;
    if (ext_ts) {
        writeBE32(hdr + hdr_len, msg.timestamp); hdr_len += 4;
    }

    const size_t total = msg.payload.size();
    size_t off = 0;
    const size_t first_slice = std::min<size_t>(out_chunk_size_, total);

    // 把 header + 首块 payload 一次性 send 出去，减少系统调用
    std::vector<uint8_t> buf;
    buf.reserve(hdr_len + first_slice);
    buf.insert(buf.end(), hdr, hdr + hdr_len);
    if (first_slice > 0) {
        buf.insert(buf.end(), msg.payload.begin(), msg.payload.begin() + first_slice);
    }
    if (s.sendAll(buf.data(), buf.size(), send_timeout_ms) != static_cast<ssize_t>(buf.size())) {
        return false;
    }
    off += first_slice;
    ++chunks_out_;
    bytes_out_ += buf.size();

    // 3. 后续 chunk 全部 Format 3：只有 1 字节 Basic Header
    //    若第一块带了 Extended Timestamp，后续每个 Format 3 也必须带（spec §5.3.1.3）
    while (off < total) {
        const size_t slice = std::min<size_t>(out_chunk_size_, total - off);
        uint8_t cont_hdr[1 + 4];
        size_t cont_len = 0;
        cont_hdr[cont_len++] = static_cast<uint8_t>((3 << 6) | (msg.csid & 0x3F));  // fmt=3
        if (ext_ts) {
            writeBE32(cont_hdr + cont_len, msg.timestamp); cont_len += 4;
        }

        std::vector<uint8_t> cbuf;
        cbuf.reserve(cont_len + slice);
        cbuf.insert(cbuf.end(), cont_hdr, cont_hdr + cont_len);
        cbuf.insert(cbuf.end(), msg.payload.begin() + off, msg.payload.begin() + off + slice);
        if (s.sendAll(cbuf.data(), cbuf.size(), send_timeout_ms) != static_cast<ssize_t>(cbuf.size())) {
            return false;
        }
        off += slice;
        ++chunks_out_;
        bytes_out_ += cbuf.size();
    }
    return true;
}

// =============================== Decoder ===============================

ChunkStreamDecoder::ChunkStreamDecoder() = default;

void ChunkStreamDecoder::setInChunkSize(uint32_t size) {
    if (size < 1) size = 1;
    in_chunk_size_ = size;
}

bool ChunkStreamDecoder::feed(const uint8_t* data, size_t len, std::vector<RtmpMessage>* out) {
    if (!out) return false;
    bytes_in_ += len;
    buffer_.insert(buffer_.end(), data, data + len);

    while (true) {
        const int n = parseOneChunk(buffer_.data(), buffer_.size(), out);
        if (n < 0) return false;
        if (n == 0) break;  // 需要更多字节
        buffer_.erase(buffer_.begin(), buffer_.begin() + n);
    }
    return true;
}

int ChunkStreamDecoder::parseOneChunk(const uint8_t* data, size_t len,
                                      std::vector<RtmpMessage>* out_messages) {
    if (len < 1) return 0;
    size_t off = 0;

    // Basic Header（最多 3 字节）
    const uint8_t b0 = data[off];
    const uint8_t fmt = (b0 >> 6) & 0x03;
    uint32_t csid = b0 & 0x3F;
    if (csid == 0) {
        if (len < 2) return 0;
        csid = uint32_t(data[1]) + 64;
        off += 2;
    } else if (csid == 1) {
        if (len < 3) return 0;
        csid = uint32_t(data[1]) + (uint32_t(data[2]) << 8) + 64;
        off += 3;
    } else {
        off += 1;
    }

    CsState& st = cs_states_[csid];

    // Message Header 尺寸与字段
    uint32_t ts_or_delta = 0;
    uint32_t msg_len = st.msg_length;
    uint8_t  type_id = st.msg_type_id;
    uint32_t stream_id = st.msg_stream_id;
    size_t header_need = 0;
    switch (fmt) {
        case 0: header_need = 11; break;
        case 1: header_need = 7;  break;
        case 2: header_need = 3;  break;
        case 3: header_need = 0;  break;
    }
    if (len < off + header_need) return 0;
    if (fmt == 0) {
        ts_or_delta = readBE24(data + off);
        msg_len     = readBE24(data + off + 3);
        type_id     = data[off + 6];
        stream_id   = readLE32(data + off + 7);
        off += 11;
    } else if (fmt == 1) {
        ts_or_delta = readBE24(data + off);
        msg_len     = readBE24(data + off + 3);
        type_id     = data[off + 6];
        off += 7;
    } else if (fmt == 2) {
        ts_or_delta = readBE24(data + off);
        off += 3;
    }
    // fmt == 3: 0 byte

    // Extended Timestamp（4 字节）条件：
    // - fmt 0/1/2 且 ts_field == 0xFFFFFF → 后面跟 4 字节
    // - fmt 3 且 上一块的 ts 曾出现 Extended Timestamp → 继续带
    bool need_ext_ts = false;
    if (fmt != 3) {
        if (ts_or_delta == 0xFFFFFF) need_ext_ts = true;
    } else {
        // spec §5.3.1.3：fmt=3 只在上一块也用过 Extended 时才跟
        if (st.has_last_fmt0_or_1 && st.timestamp_delta == 0xFFFFFF) need_ext_ts = true;
    }
    if (need_ext_ts) {
        if (len < off + 4) return 0;
        ts_or_delta = readBE32(data + off);
        off += 4;
    }

    // 更新 CsState：
    // - fmt=0：绝对时间戳
    // - fmt=1/2：delta，累加到上次绝对时间戳
    // - fmt=3：继承之前 delta（同消息内多 chunk 不累加，但新消息内要累加）
    uint32_t new_abs_ts = st.timestamp;
    if (fmt == 0) {
        new_abs_ts = ts_or_delta;
        st.timestamp_delta = ts_or_delta;
        st.has_last_fmt0_or_1 = true;
    } else if (fmt == 1 || fmt == 2) {
        st.timestamp_delta = ts_or_delta;
        new_abs_ts = st.timestamp + ts_or_delta;
        st.has_last_fmt0_or_1 = true;
    } else {
        // fmt 3：只在本条消息刚开始（partial empty）时才应用 delta
        if (st.partial.empty()) {
            new_abs_ts = st.timestamp + st.timestamp_delta;
        }
    }
    st.msg_length    = msg_len;
    st.msg_type_id   = type_id;
    st.msg_stream_id = stream_id;
    st.timestamp     = new_abs_ts;

    // 计算本 chunk 的 payload 切片大小 = min(chunk_size, 剩余未收到的字节)
    const size_t already = st.partial.size();
    if (msg_len < already) return -1;
    const size_t remain  = msg_len - already;
    const size_t slice   = std::min<size_t>(remain, in_chunk_size_);
    if (len < off + slice) return 0;

    st.partial.insert(st.partial.end(), data + off, data + off + slice);
    off += slice;

    // 完整消息到达 → 输出
    if (st.partial.size() == msg_len) {
        RtmpMessage m;
        m.csid          = csid;
        m.type_id       = type_id;
        m.msg_stream_id = stream_id;
        m.timestamp     = new_abs_ts;
        m.payload       = std::move(st.partial);
        st.partial.clear();
        out_messages->push_back(std::move(m));
        ++messages_in_;
    }
    return static_cast<int>(off);
}

}  // namespace rtsp
