// 端到端：启一个极简 RTMP server，驱动 RtmpPublisher 走完整握手 + connect +
// createStream + publish + metadata + seq header + 视频帧流程，验证收到的字节合法。

#include <rtsp-rtmp/rtsp-rtmp.h>
#include <rtsp-common/socket.h>
#include <rtsp-common/common.h>

#include "amf0_codec.h"
#include "rtmp_chunk_stream.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// CHECK：等价于 assert，但不受 NDEBUG 影响（Release 也会执行和检查）。
// 这个 test 有很多带副作用的表达式（如 pub.pushH264Data(...)），放在 assert()
// 里在 Release/NDEBUG 下会被整个剥掉，测试变成无效。
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ \
                  << ": " << #expr << std::endl; \
        std::abort(); \
    } \
} while (0)

using namespace rtsp;
using namespace std::chrono_literals;

namespace {

// ------------ Minimal RTMP mock server ------------

class MockRtmpServer {
public:
    bool start(uint16_t port) {
        if (!listen_.bind("127.0.0.1", port)) return false;
        if (!listen_.listen(1)) return false;
        if (!listen_.setNonBlocking(true)) return false;
        port_ = port;
        running_.store(true);
        th_ = std::thread([this] { runOnce(); });
        return true;
    }
    void stop() {
        running_.store(false);
        listen_.shutdownReadWrite();
        listen_.close();
        if (th_.joinable()) th_.join();
    }

    // 等待 publish 完成（即 publisher 发完 metadata + seq + 至少一帧）
    bool waitForFrames(int min_frames, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_until(lk, deadline, [&] { return video_frames_ >= min_frames; });
    }

    // 供断言：收到的 onMetaData / seq header / frames 数
    struct Result {
        bool handshake_done = false;
        bool got_connect = false;
        std::string tc_url;
        std::string app;
        uint32_t assigned_stream_id = 0;
        std::string publish_stream_key;
        std::string publish_type;        // "live"
        bool got_on_meta_data = false;
        double meta_width = 0, meta_height = 0;
        std::string meta_codec_str;
        double meta_codec_num = 0;
        bool got_avc_seq_header = false;
        int video_frames = 0;
        // 第一个视频帧的首字节（便于断言 0x17/0x27/0x1C/0x90 等）
        uint8_t first_video_byte = 0;
    };
    Result snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return r_;
    }

private:
    void runOnce() {
        // accept 一个连接即可
        std::unique_ptr<Socket> client;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (running_.load() && std::chrono::steady_clock::now() < deadline) {
            auto c = listen_.accept();
            if (c) { client = std::move(c); break; }
            std::this_thread::sleep_for(20ms);
        }
        if (!client) return;
        client->setNonBlocking(false);
        client->setTcpNoDelay(true);

        // ---------- Handshake ----------
        // 收 C0+C1（1 + 1536）
        std::vector<uint8_t> c0c1(1 + 1536);
        if (!recvExact(*client, c0c1.data(), c0c1.size(), 3000)) return;
        if (c0c1[0] != 0x03) return;

        // 发 S0+S1+S2
        std::vector<uint8_t> s0(1, 0x03);
        std::vector<uint8_t> s1(1536);
        std::vector<uint8_t> s2(c0c1.begin() + 1, c0c1.end());  // S2 = C1
        // S1 前 4 字节 timestamp=0，后面随便填点内容（测试不校验）
        std::memset(s1.data(), 0, 8);
        for (size_t i = 8; i < s1.size(); ++i) s1[i] = static_cast<uint8_t>(i & 0xFF);

        std::vector<uint8_t> s0s1s2;
        s0s1s2.reserve(s0.size() + s1.size() + s2.size());
        s0s1s2.insert(s0s1s2.end(), s0.begin(), s0.end());
        s0s1s2.insert(s0s1s2.end(), s1.begin(), s1.end());
        s0s1s2.insert(s0s1s2.end(), s2.begin(), s2.end());
        if (client->sendAll(s0s1s2.data(), s0s1s2.size(), 3000) !=
            static_cast<ssize_t>(s0s1s2.size())) return;

        // 收 C2 (1536)
        std::vector<uint8_t> c2(1536);
        if (!recvExact(*client, c2.data(), c2.size(), 3000)) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            r_.handshake_done = true;
        }

        // ---------- 之后都走 chunk stream ----------
        ChunkStreamDecoder dec;
        ChunkStreamEncoder enc;

        auto reply = [&](const RtmpMessage& m) {
            return enc.writeMessage(*client, m, 3000);
        };
        auto buildCommandResult = [](const std::string& name,
                                     double tx_id,
                                     amf0::ValuePtr arg3,
                                     amf0::ValuePtr arg4) {
            std::vector<uint8_t> body;
            amf0::encode(body, *amf0::Value::makeString(name));
            amf0::encode(body, *amf0::Value::makeNumber(tx_id));
            amf0::encode(body, *arg3);
            amf0::encode(body, *arg4);
            return body;
        };

        auto loopStart = std::chrono::steady_clock::now();
        uint8_t buf[4096];
        while (running_.load() &&
               std::chrono::steady_clock::now() - loopStart < std::chrono::seconds(10)) {
            ssize_t n = client->recv(buf, sizeof(buf), 200);
            if (n > 0) {
                std::vector<RtmpMessage> msgs;
                if (!dec.feed(buf, static_cast<size_t>(n), &msgs)) return;
                for (auto& m : msgs) {
                    handleMessage(m, dec, enc, *client, reply, buildCommandResult);
                    if (video_frames_ >= 3) goto done;  // 收够就结束
                }
            } else if (n == 0) {
                return;  // peer closed
            }
        }
    done:
        return;
    }

    void handleMessage(const RtmpMessage& m,
                       ChunkStreamDecoder& dec,
                       ChunkStreamEncoder& enc,
                       Socket& client,
                       std::function<bool(const RtmpMessage&)> reply,
                       std::function<std::vector<uint8_t>(const std::string&, double,
                                                           amf0::ValuePtr, amf0::ValuePtr)> buildRes) {
        switch (m.type_id) {
            case rtmp_msg::kSetChunkSize: {
                if (m.payload.size() >= 4) {
                    const uint32_t sz = (uint32_t(m.payload[0]) << 24) |
                                        (uint32_t(m.payload[1]) << 16) |
                                        (uint32_t(m.payload[2]) << 8)  |
                                         uint32_t(m.payload[3]);
                    dec.setInChunkSize(sz);
                }
                return;
            }
            case rtmp_msg::kCommandAmf0: {
                std::vector<amf0::Value> vs;
                amf0::parseValues(m.payload.data(), m.payload.size(), &vs);
                if (vs.size() < 2) return;
                const std::string cmd = vs[0].asString();
                const double tx = vs[1].asNumber();

                if (cmd == "connect") {
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        r_.got_connect = true;
                        if (vs.size() >= 3 && vs[2].type == amf0::Type::Object) {
                            auto* app = vs[2].getProp("app");
                            auto* tc  = vs[2].getProp("tcUrl");
                            if (app) r_.app = app->asString();
                            if (tc)  r_.tc_url = tc->asString();
                        }
                    }
                    // 先回一个 Window Ack Size 比较规范
                    RtmpMessage wack;
                    wack.csid = rtmp_csid::kProtocolControl;
                    wack.type_id = rtmp_msg::kWindowAckSize;
                    wack.payload = {0x00, 0x4C, 0x4B, 0x40};  // 5,000,000
                    reply(wack);
                    // _result with NetConnection.Connect.Success
                    auto props = amf0::Value::makeObject();
                    props->addString("fmsVer", "FMS/3,0,1,123");
                    props->addNumber("capabilities", 31.0);
                    auto info = amf0::Value::makeObject();
                    info->addString("level", "status");
                    info->addString("code", "NetConnection.Connect.Success");
                    info->addString("description", "Connection succeeded.");
                    info->addNumber("objectEncoding", 0);
                    auto body = buildRes("_result", tx, props, info);
                    RtmpMessage res;
                    res.csid = rtmp_csid::kInvoke;
                    res.type_id = rtmp_msg::kCommandAmf0;
                    res.payload = std::move(body);
                    reply(res);
                    return;
                }
                if (cmd == "createStream") {
                    const double assigned = 1.0;
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        r_.assigned_stream_id = static_cast<uint32_t>(assigned);
                    }
                    auto null_val = amf0::Value::makeNull();
                    auto stream_id = amf0::Value::makeNumber(assigned);
                    auto body = buildRes("_result", tx, null_val, stream_id);
                    RtmpMessage res;
                    res.csid = rtmp_csid::kInvoke;
                    res.type_id = rtmp_msg::kCommandAmf0;
                    res.payload = std::move(body);
                    reply(res);
                    return;
                }
                if (cmd == "publish") {
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        if (vs.size() >= 4) r_.publish_stream_key = vs[3].asString();
                        if (vs.size() >= 5) r_.publish_type = vs[4].asString();
                    }
                    // onStatus NetStream.Publish.Start
                    auto null_val = amf0::Value::makeNull();
                    auto info = amf0::Value::makeObject();
                    info->addString("level", "status");
                    info->addString("code", "NetStream.Publish.Start");
                    info->addString("description", "Start publishing.");
                    std::vector<uint8_t> body;
                    amf0::encode(body, *amf0::Value::makeString("onStatus"));
                    amf0::encode(body, *amf0::Value::makeNumber(0));
                    amf0::encode(body, *null_val);
                    amf0::encode(body, *info);
                    RtmpMessage res;
                    res.csid = rtmp_csid::kInvoke;
                    res.type_id = rtmp_msg::kCommandAmf0;
                    res.msg_stream_id = m.msg_stream_id;
                    res.payload = std::move(body);
                    reply(res);
                    return;
                }
                // FCPublish / FCUnpublish / releaseStream 忽略（publisher 不等结果）
                return;
            }
            case rtmp_msg::kDataAmf0: {
                // @setDataFrame / onMetaData
                std::vector<amf0::Value> vs;
                amf0::parseValues(m.payload.data(), m.payload.size(), &vs);
                std::string first = vs.empty() ? "" : vs[0].asString();
                std::string second = vs.size() < 2 ? "" : vs[1].asString();
                amf0::Value* meta = nullptr;
                if (first == "@setDataFrame" && second == "onMetaData" && vs.size() >= 3) {
                    meta = &vs[2];
                } else if (first == "onMetaData" && vs.size() >= 2) {
                    meta = &vs[1];
                }
                if (meta) {
                    std::lock_guard<std::mutex> lk(mu_);
                    r_.got_on_meta_data = true;
                    if (auto* w = meta->getProp("width"))  r_.meta_width  = w->asNumber();
                    if (auto* h = meta->getProp("height")) r_.meta_height = h->asNumber();
                    if (auto* c = meta->getProp("videocodecid")) {
                        if (c->type == amf0::Type::String) r_.meta_codec_str = c->str;
                        else if (c->type == amf0::Type::Number) r_.meta_codec_num = c->num;
                    }
                }
                return;
            }
            case rtmp_msg::kVideo: {
                if (m.payload.empty()) return;
                std::lock_guard<std::mutex> lk(mu_);
                // AVC seq header 判断：首字节 0x17 且第二字节 0x00（AVCPacketType=0）
                if (m.payload.size() >= 2 &&
                    ((m.payload[0] & 0x0F) == 0x07) &&
                    m.payload[1] == 0x00) {
                    r_.got_avc_seq_header = true;
                } else if (m.payload[0] == 0x90) {
                    // H.265 Enhanced seq start
                    r_.got_avc_seq_header = true;
                } else {
                    // 普通视频帧
                    if (video_frames_ == 0) r_.first_video_byte = m.payload[0];
                    ++video_frames_;
                    r_.video_frames = video_frames_;
                    cv_.notify_all();
                }
                return;
            }
        }
    }

    bool recvExact(Socket& s, uint8_t* out, size_t n, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        size_t off = 0;
        while (off < n && std::chrono::steady_clock::now() < deadline) {
            int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            if (remain < 1) remain = 1;
            ssize_t r = s.recv(out + off, n - off, std::min(200, remain));
            if (r > 0) off += static_cast<size_t>(r);
            else if (r == 0) return false;
        }
        return off == n;
    }

    Socket listen_;
    std::thread th_;
    std::atomic<bool> running_{false};
    uint16_t port_ = 0;

    std::mutex mu_;
    std::condition_variable cv_;
    int video_frames_ = 0;
    Result r_;
};

// 造一个"完整"的 H.264 IDR Annex-B 帧（SPS+PPS+IDR，共 3 个 NALU）
std::vector<uint8_t> makeH264Idr() {
    std::vector<uint8_t> out;
    // SPS
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F, 0xD9, 0x00, 0x78, 0x02, 0x27, 0xE5, 0xC0, 0x44});
    // PPS
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80});
    // IDR（简化）
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80, 0x00, 0x00, 0x00, 0x00, 0x10});
    return out;
}
std::vector<uint8_t> makeH264Inter() {
    std::vector<uint8_t> out = {0x00, 0x00, 0x00, 0x01, 0x41};
    out.insert(out.end(), 50, 0x33);
    return out;
}

}  // namespace

int main() {
    const uint16_t kPort = 18931;
    MockRtmpServer srv;
    if (!srv.start(kPort)) {
        std::cerr << "SKIP: cannot bind mock server port" << std::endl;
        return 0;
    }

    // 在独立线程里跑 publisher（mock server 已经 accept 等待）
    RtmpPublisher pub;
    RtmpPublishConfig cfg;
    cfg.connect_timeout_ms = 3000;
    cfg.handshake_timeout_ms = 3000;
    cfg.send_timeout_ms = 3000;
    cfg.out_chunk_size = 4096;
    pub.setConfig(cfg);

    RtmpPublishMediaInfo media;
    media.codec = CodecType::H264;
    media.width = 1920;
    media.height = 1080;
    media.fps = 30;
    media.bitrate_kbps = 3000;

    const std::string url = "rtmp://127.0.0.1:" + std::to_string(kPort) + "/live/testkey";
    if (!pub.open(url, media)) {
        std::cerr << "FAIL: open failed: " << pub.getLastError() << std::endl;
        srv.stop();
        return 1;
    }

    // 推 3 帧（1 关键 + 2 普通）
    const auto idr = makeH264Idr();
    const auto p   = makeH264Inter();
    CHECK(pub.pushH264Data(idr.data(), idr.size(), 0, true));
    std::this_thread::sleep_for(30ms);
    CHECK(pub.pushH264Data(p.data(), p.size(), 33, false));
    std::this_thread::sleep_for(30ms);
    CHECK(pub.pushH264Data(p.data(), p.size(), 66, false));

    // 等 server 收到至少 2 帧（第一帧是 key，server 会分开计）
    const bool got_frames = srv.waitForFrames(2, 3000);
    CHECK(got_frames);

    // 断言
    const auto r = srv.snapshot();
    std::cout << "handshake=" << r.handshake_done
              << " connect=" << r.got_connect
              << " tcUrl=" << r.tc_url
              << " app=" << r.app
              << " stream_id=" << r.assigned_stream_id
              << " key=" << r.publish_stream_key
              << " type=" << r.publish_type
              << " onMetaData=" << r.got_on_meta_data
              << " w=" << r.meta_width << " h=" << r.meta_height
              << " codec_num=" << r.meta_codec_num
              << " seq=" << r.got_avc_seq_header
              << " frames=" << r.video_frames
              << " first_byte=0x" << std::hex << int(r.first_video_byte) << std::dec
              << std::endl;

    CHECK(r.handshake_done);
    CHECK(r.got_connect);
    CHECK(r.tc_url == "rtmp://127.0.0.1:" + std::to_string(kPort) + "/live");
    CHECK(r.app == "live");
    CHECK(r.assigned_stream_id == 1);
    CHECK(r.publish_stream_key == "testkey");
    CHECK(r.publish_type == "live");
    CHECK(r.got_on_meta_data);
    CHECK(r.meta_width == 1920);
    CHECK(r.meta_height == 1080);
    CHECK(r.meta_codec_num == 7);    // H.264 codec id
    CHECK(r.got_avc_seq_header);
    CHECK(r.video_frames >= 2);
    // 首帧 is_key=true → 0x17
    CHECK(r.first_video_byte == 0x17);

    const auto st = pub.getStats();
    std::cout << "stats: messages_sent=" << st.messages_sent
              << " frames_sent=" << st.video_frames_sent
              << " bytes_sent=" << st.bytes_sent
              << " chunks=" << st.chunk_count << std::endl;
    CHECK(st.video_frames_sent == 3);

    pub.closeWithTimeout(1000);
    srv.stop();
    std::cout << "rtmp publisher integration test passed\n";
    return 0;
}
