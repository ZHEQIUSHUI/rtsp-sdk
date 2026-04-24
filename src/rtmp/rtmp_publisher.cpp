#include <rtsp-rtmp/rtmp_publisher.h>

#include "amf0_codec.h"
#include "avc_config_record.h"
#include "flv_tag_encoder.h"
#include "hevc_config_record.h"
#include "rtmp_chunk_stream.h"
#include "rtmp_handshake.h"

#include <rtsp-common/common.h>
#include <rtsp-common/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

namespace rtsp {

namespace {

// 从 rtmp://host[:port]/app[/streamkey[?...]] 里拆出 host/port/app/streamkey。
// streamkey 允许包含 '/' 与查询串（抖音/快手的 streamkey 常有 '?sign=...'）。
bool parseRtmpUrl(const std::string& url,
                  std::string* host, uint16_t* port,
                  std::string* app, std::string* stream_key,
                  std::string* tc_url) {
    const std::string prefix = "rtmp://";
    if (url.rfind(prefix, 0) != 0) return false;
    const std::string rest = url.substr(prefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return false;
    const std::string hostport = rest.substr(0, slash);
    std::string path = rest.substr(slash + 1);

    const size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        *host = hostport.substr(0, colon);
        try {
            const int p = std::stoi(hostport.substr(colon + 1));
            if (p <= 0 || p > 65535) return false;
            *port = static_cast<uint16_t>(p);
        } catch (...) { return false; }
    } else {
        *host = hostport;
        *port = 1935;
    }
    if (host->empty()) return false;

    const size_t sep = path.find('/');
    if (sep == std::string::npos) {
        *app = path;
        *stream_key = "";
    } else {
        *app = path.substr(0, sep);
        *stream_key = path.substr(sep + 1);
    }
    if (app->empty()) return false;

    *tc_url = "rtmp://" + hostport + "/" + *app;
    return true;
}

// NALU 扫描：抽出 H.264 SPS/PPS，H.265 VPS/SPS/PPS，用于 sequence header。
struct NaluScan264 {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    bool has_sps = false;
    bool has_pps = false;
};
struct NaluScan265 {
    std::vector<uint8_t> vps;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    bool has_vps = false;
    bool has_sps = false;
    bool has_pps = false;
};

bool isStart3(const uint8_t* p, size_t rem) {
    return rem >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1;
}
bool isStart4(const uint8_t* p, size_t rem) {
    return rem >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}

template <typename Fn>
void forEachNalu(const uint8_t* data, size_t len, Fn&& fn) {
    size_t i = 0;
    while (i < len) {
        size_t sc = 0;
        if (isStart4(data + i, len - i)) sc = 4;
        else if (isStart3(data + i, len - i)) sc = 3;
        if (sc == 0) return;
        i += sc;
        size_t j = i;
        while (j < len) {
            if (isStart3(data + j, len - j) || isStart4(data + j, len - j)) break;
            ++j;
        }
        if (j > i) fn(data + i, j - i);
        i = j;
    }
}

NaluScan264 scan264(const uint8_t* data, size_t len) {
    NaluScan264 s;
    forEachNalu(data, len, [&](const uint8_t* p, size_t n) {
        if (n == 0) return;
        const uint8_t t = p[0] & 0x1F;
        if (t == 7) { s.sps.assign(p, p + n); s.has_sps = true; }
        else if (t == 8) { s.pps.assign(p, p + n); s.has_pps = true; }
    });
    return s;
}
NaluScan265 scan265(const uint8_t* data, size_t len) {
    NaluScan265 s;
    forEachNalu(data, len, [&](const uint8_t* p, size_t n) {
        if (n < 2) return;
        const uint8_t t = (p[0] >> 1) & 0x3F;
        if (t == 32) { s.vps.assign(p, p + n); s.has_vps = true; }
        else if (t == 33) { s.sps.assign(p, p + n); s.has_sps = true; }
        else if (t == 34) { s.pps.assign(p, p + n); s.has_pps = true; }
    });
    return s;
}

}  // namespace

// ========================== Impl ==========================

class RtmpPublisher::Impl {
public:
    RtmpPublishConfig cfg_;
    std::unique_ptr<Socket> socket_;
    ChunkStreamEncoder enc_;
    ChunkStreamDecoder dec_;

    // 解析后的 URL 信息
    std::string host_;
    uint16_t port_ = 1935;
    std::string app_;
    std::string stream_key_;
    std::string tc_url_;

    // 媒体元信息
    RtmpPublishMediaInfo media_;

    // 状态
    bool connected_ = false;
    uint32_t next_transaction_id_ = 1;
    uint32_t publish_stream_id_ = 0;
    bool seq_header_sent_ = false;
    uint64_t base_pts_ms_ = 0;
    bool base_pts_inited_ = false;
    std::string last_error_;

    // stats
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> bytes_sent_{0};

    // ---------- helpers ----------

    void setErr(std::string e) {
        RTSP_LOG_WARNING("RtmpPublisher: " + e);
        last_error_ = std::move(e);
    }

    // 以 uint32 ms 形式裁剪 pts（RTMP spec timestamp 是 uint32，但实际多数服务器
    // 只读 3B；我们 writeBE24 + extended timestamp 都处理好了）
    uint32_t relTs(uint64_t pts_ms) {
        if (!base_pts_inited_) {
            base_pts_ms_ = pts_ms;
            base_pts_inited_ = true;
        }
        if (pts_ms < base_pts_ms_) return 0;
        return static_cast<uint32_t>(pts_ms - base_pts_ms_);
    }

    // 发一条消息（包装 chunk stream）
    bool sendMessage(const RtmpMessage& m) {
        if (!socket_ || !socket_->isValid()) return false;
        if (!enc_.writeMessage(*socket_, m, static_cast<int>(cfg_.send_timeout_ms))) {
            setErr("send message failed (type=" + std::to_string(m.type_id) + ")");
            return false;
        }
        messages_sent_.fetch_add(1);
        bytes_sent_.fetch_add(m.payload.size());
        return true;
    }

    // 发 Set Chunk Size
    bool sendSetChunkSize(uint32_t size) {
        RtmpMessage m;
        m.csid = rtmp_csid::kProtocolControl;
        m.type_id = rtmp_msg::kSetChunkSize;
        m.msg_stream_id = 0;
        m.payload = {
            static_cast<uint8_t>((size >> 24) & 0xFF),
            static_cast<uint8_t>((size >> 16) & 0xFF),
            static_cast<uint8_t>((size >> 8)  & 0xFF),
            static_cast<uint8_t>( size        & 0xFF),
        };
        if (!sendMessage(m)) return false;
        enc_.setOutChunkSize(size);
        return true;
    }

    // 发 Window Acknowledgement Size
    bool sendWindowAckSize(uint32_t size) {
        RtmpMessage m;
        m.csid = rtmp_csid::kProtocolControl;
        m.type_id = rtmp_msg::kWindowAckSize;
        m.msg_stream_id = 0;
        m.payload = {
            static_cast<uint8_t>((size >> 24) & 0xFF),
            static_cast<uint8_t>((size >> 16) & 0xFF),
            static_cast<uint8_t>((size >> 8)  & 0xFF),
            static_cast<uint8_t>( size        & 0xFF),
        };
        return sendMessage(m);
    }

    bool sendCommand(uint32_t csid, uint32_t msg_stream_id,
                     std::vector<uint8_t> payload) {
        RtmpMessage m;
        m.csid = csid;
        m.type_id = rtmp_msg::kCommandAmf0;
        m.msg_stream_id = msg_stream_id;
        m.payload = std::move(payload);
        return sendMessage(m);
    }

    bool sendData(uint32_t csid, uint32_t msg_stream_id,
                  std::vector<uint8_t> payload) {
        RtmpMessage m;
        m.csid = csid;
        m.type_id = rtmp_msg::kDataAmf0;
        m.msg_stream_id = msg_stream_id;
        m.payload = std::move(payload);
        return sendMessage(m);
    }

    // 等待指定 command name 的 _result（或 _error）。timeout_ms 总时限。
    // 成功时把 payload 的 AMF0 values 写到 out_values 并返回 true。
    bool waitForCommandResult(uint32_t expect_tx_id,
                              std::vector<amf0::Value>* out_values,
                              int timeout_ms) {
        if (!socket_) return false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        std::vector<RtmpMessage> msgs;

        uint8_t buf[4096];
        while (std::chrono::steady_clock::now() < deadline) {
            const int remain = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count());
            const int poll_to = std::min(200, std::max(1, remain));
            ssize_t n = socket_->recv(buf, sizeof(buf), poll_to);
            if (n > 0) {
                if (!dec_.feed(buf, static_cast<size_t>(n), &msgs)) {
                    setErr("chunk decode error");
                    return false;
                }
            } else if (n == 0) {
                setErr("peer closed during wait for command result");
                return false;
            }
            // 扫描已到消息
            for (auto it = msgs.begin(); it != msgs.end(); ) {
                const auto& m = *it;
                if (m.type_id == rtmp_msg::kSetChunkSize && m.payload.size() >= 4) {
                    const uint32_t sz = (uint32_t(m.payload[0]) << 24) |
                                        (uint32_t(m.payload[1]) << 16) |
                                        (uint32_t(m.payload[2]) << 8)  |
                                         uint32_t(m.payload[3]);
                    dec_.setInChunkSize(sz);
                    it = msgs.erase(it);
                    continue;
                }
                if (m.type_id == rtmp_msg::kCommandAmf0) {
                    std::vector<amf0::Value> vs;
                    amf0::parseValues(m.payload.data(), m.payload.size(), &vs);
                    if (vs.size() >= 2 &&
                        vs[0].type == amf0::Type::String &&
                        vs[1].type == amf0::Type::Number) {
                        const std::string cmd = vs[0].str;
                        const uint32_t tx = static_cast<uint32_t>(vs[1].num);
                        if (tx == expect_tx_id) {
                            if (cmd == "_error") {
                                std::string reason;
                                if (vs.size() >= 4) reason = vs[3].asString();
                                setErr("server responded _error: " + reason);
                                return false;
                            }
                            if (cmd == "_result") {
                                if (out_values) *out_values = std::move(vs);
                                return true;
                            }
                            // 其他响应（如 onStatus）归入下面分支
                        }
                        if (cmd == "onStatus" && expect_tx_id == 0) {
                            // publish 之后 server 会主动发 onStatus；
                            // 当调用方 pass expect_tx_id=0 时，这就是等待的目标
                            if (out_values) *out_values = std::move(vs);
                            return true;
                        }
                    }
                }
                it = msgs.erase(it);
            }
        }
        setErr("timeout waiting for command result");
        return false;
    }

    // 单条 command 请求并等待 _result
    bool doCommand(const std::string& name,
                   uint32_t tx_id,
                   const std::vector<amf0::ValuePtr>& args,
                   uint32_t csid,
                   uint32_t msg_stream_id,
                   std::vector<amf0::Value>* out_values,
                   int timeout_ms) {
        std::vector<uint8_t> body;
        amf0::encode(body, *amf0::Value::makeString(name));
        amf0::encode(body, *amf0::Value::makeNumber(tx_id));
        for (const auto& v : args) amf0::encode(body, *v);

        if (!sendCommand(csid, msg_stream_id, std::move(body))) return false;
        return waitForCommandResult(tx_id, out_values, timeout_ms);
    }

    bool doConnect(int timeout_ms) {
        auto cmd_obj = amf0::Value::makeObject();
        cmd_obj->addString("app", app_);
        cmd_obj->addString("type", "nonprivate");
        cmd_obj->addString("flashVer", cfg_.flash_ver);
        cmd_obj->addString("tcUrl", tc_url_);
        cmd_obj->addBool("fpad", false);
        cmd_obj->addNumber("capabilities", 239);
        cmd_obj->addNumber("audioCodecs", 0);
        cmd_obj->addNumber("videoCodecs", 252);  // 广泛兼容
        cmd_obj->addNumber("videoFunction", 1);
        cmd_obj->addNumber("objectEncoding", 0);

        const uint32_t tx = next_transaction_id_++;
        std::vector<amf0::ValuePtr> args = { cmd_obj };
        std::vector<amf0::Value> out;
        return doCommand("connect", tx, args, rtmp_csid::kInvoke, 0, &out, timeout_ms);
    }

    bool doReleaseStream(int /*timeout_ms*/) {
        const uint32_t tx = next_transaction_id_++;
        // 部分 server 完全不回（如 mediamtx 对 releaseStream），不强要求，所以
        // 这里不等 _result，timeout 参数只为签名一致性保留
        std::vector<uint8_t> body;
        amf0::encode(body, *amf0::Value::makeString("releaseStream"));
        amf0::encode(body, *amf0::Value::makeNumber(tx));
        amf0::encode(body, *amf0::Value::makeNull());
        amf0::encode(body, *amf0::Value::makeString(stream_key_));
        return sendCommand(rtmp_csid::kInvoke, 0, std::move(body));
    }

    bool doFCPublish(int /*timeout_ms*/) {
        const uint32_t tx = next_transaction_id_++;
        std::vector<uint8_t> body;
        amf0::encode(body, *amf0::Value::makeString("FCPublish"));
        amf0::encode(body, *amf0::Value::makeNumber(tx));
        amf0::encode(body, *amf0::Value::makeNull());
        amf0::encode(body, *amf0::Value::makeString(stream_key_));
        return sendCommand(rtmp_csid::kInvoke, 0, std::move(body));
    }

    bool doCreateStream(int timeout_ms) {
        const uint32_t tx = next_transaction_id_++;
        std::vector<amf0::ValuePtr> args = { amf0::Value::makeNull() };
        std::vector<amf0::Value> out;
        if (!doCommand("createStream", tx, args, rtmp_csid::kInvoke, 0, &out, timeout_ms)) {
            return false;
        }
        // _result 的第 4 个值是 stream id（number）
        if (out.size() < 4 || out[3].type != amf0::Type::Number) {
            setErr("createStream: bad _result format");
            return false;
        }
        publish_stream_id_ = static_cast<uint32_t>(out[3].num);
        return true;
    }

    bool doPublish(int timeout_ms) {
        const uint32_t tx = next_transaction_id_++;
        std::vector<uint8_t> body;
        amf0::encode(body, *amf0::Value::makeString("publish"));
        amf0::encode(body, *amf0::Value::makeNumber(tx));
        amf0::encode(body, *amf0::Value::makeNull());
        amf0::encode(body, *amf0::Value::makeString(stream_key_));
        amf0::encode(body, *amf0::Value::makeString("live"));
        if (!sendCommand(rtmp_csid::kInvoke, publish_stream_id_, std::move(body))) return false;
        // 等 onStatus (NetStream.Publish.Start)
        std::vector<amf0::Value> out;
        return waitForCommandResult(0, &out, timeout_ms);
    }

    bool sendMetadata() {
        // @setDataFrame(onMetaData, {width, height, fps, bitrate, codecid...})
        std::vector<uint8_t> body;
        amf0::encode(body, *amf0::Value::makeString("@setDataFrame"));
        amf0::encode(body, *amf0::Value::makeString("onMetaData"));

        auto meta = std::make_shared<amf0::Value>();
        meta->type = amf0::Type::EcmaArray;
        meta->addNumber("width",    media_.width);
        meta->addNumber("height",   media_.height);
        meta->addNumber("framerate", media_.fps);
        meta->addNumber("videodatarate", media_.bitrate_kbps);
        if (media_.codec == CodecType::H264) {
            meta->addNumber("videocodecid", 7);
        } else {
            // 国内兼容模式用 12；Enhanced RTMP 用 FourCC "hvc1"
            if (cfg_.h265_mode == 1) {
                meta->addNumber("videocodecid", 12);
            } else {
                meta->addString("videocodecid", "hvc1");
            }
        }
        amf0::encode(body, *meta);

        return sendData(rtmp_csid::kData, publish_stream_id_, std::move(body));
    }

    bool sendSeqHeaderH264() {
        if (media_.sps.empty() || media_.pps.empty()) return false;
        const auto cfg = buildAvcDecoderConfigRecord(media_.sps, media_.pps);
        if (cfg.empty()) return false;
        const auto tag = buildFlvVideoTagH264SeqHeader(cfg);
        RtmpMessage m;
        m.csid = rtmp_csid::kVideo;
        m.type_id = rtmp_msg::kVideo;
        m.msg_stream_id = publish_stream_id_;
        m.timestamp = 0;
        m.payload = tag;
        return sendMessage(m);
    }

    bool sendSeqHeaderH265() {
        if (media_.vps.empty() || media_.sps.empty() || media_.pps.empty()) return false;
        const auto cfg = buildHevcDecoderConfigRecord(media_.vps, media_.sps, media_.pps);
        if (cfg.empty()) return false;
        const auto tag = (cfg_.h265_mode == 1)
            ? buildFlvVideoTagH265LegacySeqHeader(cfg)
            : buildFlvVideoTagH265EnhancedSeqHeader(cfg);
        RtmpMessage m;
        m.csid = rtmp_csid::kVideo;
        m.type_id = rtmp_msg::kVideo;
        m.msg_stream_id = publish_stream_id_;
        m.timestamp = 0;
        m.payload = tag;
        return sendMessage(m);
    }

    bool sendVideoFrame(const std::vector<uint8_t>& avcc_payload,
                        bool is_key, uint64_t pts_ms) {
        std::vector<uint8_t> tag;
        if (media_.codec == CodecType::H264) {
            tag = buildFlvVideoTagH264Frame(avcc_payload, is_key, 0);
        } else if (cfg_.h265_mode == 1) {
            tag = buildFlvVideoTagH265LegacyFrame(avcc_payload, is_key, 0);
        } else {
            tag = buildFlvVideoTagH265EnhancedFrame(avcc_payload, is_key, 0);
        }
        RtmpMessage m;
        m.csid = rtmp_csid::kVideo;
        m.type_id = rtmp_msg::kVideo;
        m.msg_stream_id = publish_stream_id_;
        m.timestamp = relTs(pts_ms);
        m.payload = std::move(tag);
        if (!sendMessage(m)) return false;
        frames_sent_.fetch_add(1);
        return true;
    }

    bool drainIncomingNonBlock() {
        // 把 socket 里已到但我们没主动等的消息吃掉（如 ack、user control ping），
        // 避免对端窗口耗尽。最多一次循环内读完当前可读数据。
        if (!socket_ || !socket_->isValid()) return false;
        uint8_t buf[4096];
        for (int i = 0; i < 8; ++i) {
            ssize_t n = socket_->recv(buf, sizeof(buf), 0);  // 非阻塞 poll
            if (n <= 0) break;
            std::vector<RtmpMessage> msgs;
            if (!dec_.feed(buf, static_cast<size_t>(n), &msgs)) return false;
            for (const auto& m : msgs) {
                if (m.type_id == rtmp_msg::kSetChunkSize && m.payload.size() >= 4) {
                    const uint32_t sz = (uint32_t(m.payload[0]) << 24) |
                                        (uint32_t(m.payload[1]) << 16) |
                                        (uint32_t(m.payload[2]) << 8)  |
                                         uint32_t(m.payload[3]);
                    dec_.setInChunkSize(sz);
                }
                // 其他消息（window-ack / ping / onStatus）暂时忽略
            }
        }
        return true;
    }
};

// ========================== Public API ==========================

RtmpPublisher::RtmpPublisher() : impl_(std::make_unique<Impl>()) {}
RtmpPublisher::~RtmpPublisher() { close(); }

void RtmpPublisher::setConfig(const RtmpPublishConfig& config) {
    impl_->cfg_ = config;
}

bool RtmpPublisher::open(const std::string& url, const RtmpPublishMediaInfo& media) {
    if (impl_->connected_) return false;
    impl_->media_ = media;

    if (!parseRtmpUrl(url, &impl_->host_, &impl_->port_, &impl_->app_,
                      &impl_->stream_key_, &impl_->tc_url_)) {
        impl_->setErr("invalid RTMP URL: " + url);
        return false;
    }

    impl_->socket_ = std::make_unique<Socket>();
    if (!impl_->socket_->connect(impl_->host_, impl_->port_,
                                 static_cast<int>(impl_->cfg_.connect_timeout_ms))) {
        impl_->setErr("TCP connect to " + impl_->host_ + ":" +
                      std::to_string(impl_->port_) + " failed");
        return false;
    }
    impl_->socket_->setTcpNoDelay(true);

    if (!rtmpSimpleHandshakeClient(*impl_->socket_,
                                   static_cast<int>(impl_->cfg_.handshake_timeout_ms))) {
        impl_->setErr("RTMP simple handshake failed");
        return false;
    }

    // 协议控制消息 + 命令
    if (!impl_->sendWindowAckSize(5'000'000)) return false;

    const int tmo = static_cast<int>(impl_->cfg_.handshake_timeout_ms);
    if (!impl_->doConnect(tmo)) return false;

    // 把输出 chunk size 提升，减少分片开销
    if (impl_->cfg_.out_chunk_size > 128 &&
        !impl_->sendSetChunkSize(impl_->cfg_.out_chunk_size)) return false;

    // releaseStream / FCPublish 对部分 server 是可选但无害的操作
    impl_->doReleaseStream(tmo);
    impl_->doFCPublish(tmo);

    if (!impl_->doCreateStream(tmo)) return false;
    if (!impl_->doPublish(tmo)) return false;
    if (!impl_->sendMetadata()) return false;

    // 若调用方显式提供了 SPS/PPS（和 H.265 的 VPS），立刻发 sequence header。
    // 否则等第一次 push 关键帧时自动提取并发。
    if (media.codec == CodecType::H264) {
        if (!media.sps.empty() && !media.pps.empty()) {
            if (!impl_->sendSeqHeaderH264()) return false;
            impl_->seq_header_sent_ = true;
        }
    } else {
        if (!media.vps.empty() && !media.sps.empty() && !media.pps.empty()) {
            if (!impl_->sendSeqHeaderH265()) return false;
            impl_->seq_header_sent_ = true;
        }
    }

    impl_->connected_ = true;
    impl_->last_error_.clear();
    RTSP_LOG_INFO("RtmpPublisher open OK: " + url);
    return true;
}

bool RtmpPublisher::pushH264Data(const uint8_t* data, size_t size, uint64_t pts_ms, bool is_key) {
    if (!impl_->connected_ || !data || size == 0) return false;
    impl_->drainIncomingNonBlock();

    // 自动提取 SPS/PPS（首次 IDR 时）
    if (!impl_->seq_header_sent_ && is_key) {
        const auto s = scan264(data, size);
        if (s.has_sps) impl_->media_.sps = s.sps;
        if (s.has_pps) impl_->media_.pps = s.pps;
        if (!impl_->media_.sps.empty() && !impl_->media_.pps.empty()) {
            if (!impl_->sendSeqHeaderH264()) return false;
            impl_->seq_header_sent_ = true;
        } else {
            // 还没齐全，本帧也没法正确解码，丢弃
            return false;
        }
    }
    if (!impl_->seq_header_sent_) return false;

    const auto avcc = annexBToAvcc(data, size);
    if (avcc.empty()) return false;
    return impl_->sendVideoFrame(avcc, is_key, pts_ms);
}

bool RtmpPublisher::pushH265Data(const uint8_t* data, size_t size, uint64_t pts_ms, bool is_key) {
    if (!impl_->connected_ || !data || size == 0) return false;
    impl_->drainIncomingNonBlock();

    if (!impl_->seq_header_sent_ && is_key) {
        const auto s = scan265(data, size);
        if (s.has_vps) impl_->media_.vps = s.vps;
        if (s.has_sps) impl_->media_.sps = s.sps;
        if (s.has_pps) impl_->media_.pps = s.pps;
        if (!impl_->media_.vps.empty() &&
            !impl_->media_.sps.empty() &&
            !impl_->media_.pps.empty()) {
            if (!impl_->sendSeqHeaderH265()) return false;
            impl_->seq_header_sent_ = true;
        } else {
            return false;
        }
    }
    if (!impl_->seq_header_sent_) return false;

    const auto avcc = annexBToAvcc(data, size);
    if (avcc.empty()) return false;
    return impl_->sendVideoFrame(avcc, is_key, pts_ms);
}

void RtmpPublisher::close() { (void)closeWithTimeout(3000); }

bool RtmpPublisher::closeWithTimeout(uint32_t timeout_ms) {
    if (impl_->socket_ && impl_->connected_) {
        const int tmo = static_cast<int>(timeout_ms);
        // FCUnpublish + deleteStream（best-effort；不强制等 _result）
        const uint32_t tx1 = impl_->next_transaction_id_++;
        std::vector<uint8_t> body1;
        amf0::encode(body1, *amf0::Value::makeString("FCUnpublish"));
        amf0::encode(body1, *amf0::Value::makeNumber(tx1));
        amf0::encode(body1, *amf0::Value::makeNull());
        amf0::encode(body1, *amf0::Value::makeString(impl_->stream_key_));
        impl_->sendCommand(rtmp_csid::kInvoke, 0, std::move(body1));

        const uint32_t tx2 = impl_->next_transaction_id_++;
        std::vector<uint8_t> body2;
        amf0::encode(body2, *amf0::Value::makeString("deleteStream"));
        amf0::encode(body2, *amf0::Value::makeNumber(tx2));
        amf0::encode(body2, *amf0::Value::makeNull());
        amf0::encode(body2, *amf0::Value::makeNumber(impl_->publish_stream_id_));
        impl_->sendCommand(rtmp_csid::kInvoke, 0, std::move(body2));
        (void)tmo;
    }
    if (impl_->socket_) {
        impl_->socket_->shutdownReadWrite();
        impl_->socket_->close();
        impl_->socket_.reset();
    }
    impl_->connected_ = false;
    impl_->seq_header_sent_ = false;
    impl_->base_pts_inited_ = false;
    impl_->publish_stream_id_ = 0;
    return true;
}

bool RtmpPublisher::isConnected() const { return impl_->connected_; }
std::string RtmpPublisher::getLastError() const { return impl_->last_error_; }

RtmpPublisher::Stats RtmpPublisher::getStats() const {
    Stats s;
    s.messages_sent     = impl_->messages_sent_.load();
    s.video_frames_sent = impl_->frames_sent_.load();
    s.bytes_sent        = impl_->bytes_sent_.load();
    s.chunk_count       = impl_->enc_.chunksOut();
    return s;
}

}  // namespace rtsp
