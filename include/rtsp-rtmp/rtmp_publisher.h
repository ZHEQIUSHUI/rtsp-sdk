#pragma once

// RTMP Publisher：把 H.264 / H.265 Annex-B 视频流推到外部 RTMP server
// （B 站 / 抖音 / 快手 / YouTube / mediamtx / SRS / nginx-rtmp 等）。
//
// 与 RtspPublisher 的 API 风格对齐：
//   - 构造 → setConfig → open(media) → pushH26xData(...) 循环 → close
//   - closeWithTimeout(ms) 真正遵守超时
//
// 实现范围（够用即可）：
//   - Simple handshake（C0-C2/S0-S2），兼容绝大多数 CDN / 自建服务器
//   - H.264 原生 (codecId=7, AVC)
//   - H.265：Enhanced RTMP (FourCC=hvc1) + 国内兼容 (codecId=12) 两种模式
//   - 自动从关键帧提取 SPS/PPS(/VPS) 并发送 sequence header
//   - Annex-B 自动转换为 AVCC（长度前缀）
//
// 不在本次范围内：
//   - Complex handshake（YouTube/Twitch 要求，后续可加）
//   - 音频推流
//   - Play（RtmpPublisher 不负责拉流）
//   - 断流重连（调用方用 open/close 循环实现即可）

#include <rtsp-common/common.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rtsp {

struct RtmpPublishConfig {
    // TCP 建连超时
    uint32_t connect_timeout_ms = 10000;
    // 每次 send 的写超时（防对端不收包时阻塞）
    uint32_t send_timeout_ms = 5000;
    // RTMP 握手 + 控制交互的接收超时
    uint32_t handshake_timeout_ms = 10000;
    // User-Agent / flashVer；某些 CDN 会校验
    std::string flash_ver = "FMLE/3.0 (compatible; rtsp-sdk)";
    // 发送端 chunk size。越大越少分片开销；默认 4096 是常见取值
    uint32_t out_chunk_size = 4096;
    // H.265 的载荷格式：
    //   0 = Enhanced RTMP（YouTube 2022 提案，FourCC=hvc1，国际标准）
    //   1 = 国内兼容模式（codecId=12，B 站 / 抖音 / 快手接受）
    // H.264 永远走标准 codecId=7，不受此选项影响
    int h265_mode = 0;
};

struct RtmpPublishMediaInfo {
    CodecType codec = CodecType::H264;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    // 可选：声明给 server 的目标码率（只用于 onMetaData，不做本地限速）
    uint32_t bitrate_kbps = 4000;
    // 可选：显式 SPS/PPS/VPS。若空，自动从首个 IDR 提取
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> vps;  // 仅 H.265
};

class RtmpPublisher {
public:
    RtmpPublisher();
    ~RtmpPublisher();

    RtmpPublisher(const RtmpPublisher&) = delete;
    RtmpPublisher& operator=(const RtmpPublisher&) = delete;

    void setConfig(const RtmpPublishConfig& config);

    // 建连 + 握手 + connect + createStream + publish + onMetaData。
    // url 形如 rtmp://host[:port]/app/streamkey（port 默认 1935）。
    // 返回 false 时用 getLastError() 取错误原因。
    bool open(const std::string& url, const RtmpPublishMediaInfo& media);

    // 推送 Annex-B 帧（带起始码）。内部会：
    //   1. 首次 IDR 时提取 SPS/PPS 并发送 AVC/HVC sequence header
    //   2. 将 Annex-B 转成 AVCC（4 字节长度前缀）
    //   3. 按 pts_ms 打 RTMP 时间戳
    bool pushH264Data(const uint8_t* data, size_t size, uint64_t pts_ms, bool is_key);
    bool pushH265Data(const uint8_t* data, size_t size, uint64_t pts_ms, bool is_key);

    void close();
    // 发送 FCUnpublish / deleteStream 并关 socket；遵守 timeout_ms
    bool closeWithTimeout(uint32_t timeout_ms);

    bool isConnected() const;
    std::string getLastError() const;

    struct Stats {
        uint64_t messages_sent     = 0;  // RTMP 消息（不计底层 TCP 包）
        uint64_t video_frames_sent = 0;
        uint64_t bytes_sent        = 0;
        uint64_t chunk_count       = 0;  // 实际发出的 chunk 数，用于调试
    };
    Stats getStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 别名，保持与 RtspPublisher 的 RtspPusher 对称
using RtmpPusher = RtmpPublisher;

}  // namespace rtsp
