#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>

namespace rtsp {

// 视频编码类型
enum class CodecType {
    H264,
    H265
};

// 视频帧类型
enum class FrameType {
    IDR,    // 关键帧
    P,      // P帧
    B       // B帧
};

// NALU单元
struct NaluUnit {
    uint8_t type;           // NALU类型
    const uint8_t* data;    // 数据指针
    size_t size;            // 数据大小
    bool start_code;        // 是否包含起始码
};

// 视频帧
struct VideoFrame {
    CodecType codec;        // 编码类型
    FrameType type;         // 帧类型
    uint8_t* data;          // 帧数据
    size_t size;            // 数据大小
    uint64_t pts;           // 显示时间戳 (毫秒)
    uint64_t dts;           // 解码时间戳 (毫秒)
    uint32_t width;         // 宽度
    uint32_t height;        // 高度
    uint32_t fps;           // 帧率
};

// 音频帧
struct AudioFrame {
    uint8_t* data;
    size_t size;
    uint64_t pts;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
};

// RTP包
struct RtpPacket {
    uint8_t* data;
    size_t size;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
    bool marker;
};

// SDP媒体信息
struct SdpMediaInfo {
    CodecType codec;
    std::string payload_name;
    uint8_t payload_type;
    uint32_t clock_rate;
    std::string sps;        // Base64编码的SPS
    std::string pps;        // Base64编码的PPS
    std::string vps;        // Base64编码的VPS (仅HEVC)
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

// 时间戳转换工具
inline uint32_t convertToRtpTimestamp(uint64_t pts_ms, uint32_t clock_rate) {
    return static_cast<uint32_t>((pts_ms * clock_rate) / 1000);
}

// Base64编码/解码
std::string base64Encode(const uint8_t* data, size_t size);
std::vector<uint8_t> base64Decode(const std::string& str);
std::string md5Hex(const std::string& data);

// 日志级别
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

enum class LogFormat {
    PlainText,
    Json
};

struct LogConfig {
    LogLevel min_level = LogLevel::Debug;
    LogFormat format = LogFormat::PlainText;
    bool use_utc_time = false;
    bool include_thread_id = true;
};

using LogCallback = std::function<void(LogLevel, const std::string&)>;
void setLogConfig(const LogConfig& config);
LogConfig getLogConfig();
void setLogCallback(LogCallback callback);
void log(LogLevel level, const std::string& msg);

#define RTSP_LOG_DEBUG(msg) rtsp::log(rtsp::LogLevel::Debug, msg)
#define RTSP_LOG_INFO(msg) rtsp::log(rtsp::LogLevel::Info, msg)
#define RTSP_LOG_WARNING(msg) rtsp::log(rtsp::LogLevel::Warning, msg)
#define RTSP_LOG_ERROR(msg) rtsp::log(rtsp::LogLevel::Error, msg)

} // namespace rtsp
