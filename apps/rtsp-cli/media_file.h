#pragma once

// rtsp-cli 媒体文件读写层。
//
// 统一把"文件"抽象成 Annex-B 访问单元(AU)序列：
//   - 读：裸流 .h264/.h265 或 .mp4（minimp4 解封装）→ AU(带起始码) + pts + 关键帧标志
//   - 写：裸流 .h264/.h265/stdout 或 .mp4（minimp4 封装）
// 按扩展名自动选择实现；SDK 只认 Annex-B，所以对外一律 Annex-B。

#include <rtsp-common/common.h>   // CodecType, VideoFrame

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtspcli {

// 输入流的基本参数（一次性"流信息块"用）。
struct MediaParams {
    rtsp::CodecType codec = rtsp::CodecType::H264;
    int width = 0;
    int height = 0;
    int fps = 0;                          // 估算/指定；裸流无时间戳时来自 --fps
    std::vector<uint8_t> vps;             // 裸 NAL（无起始码），仅 HEVC
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
};

// 一个访问单元（≈一帧），Annex-B（含起始码），可直接喂 pushH264Data/pushH265Data。
struct AccessUnit {
    std::vector<uint8_t> data;
    uint64_t pts_ms = 0;
    bool is_key = false;
};

// 读：裸流 / mp4。
class MediaReader {
public:
    // forced_fps>0 时覆盖估算的帧率（裸流无时间戳必需，mp4 也可强制）。
    static std::unique_ptr<MediaReader> open(const std::string& path, int forced_fps,
                                             std::string& err);
    virtual ~MediaReader() = default;
    virtual const MediaParams& params() const = 0;
    virtual bool next(AccessUnit& au) = 0;   // 返回 false 表示读到结尾
    virtual void rewind() = 0;                // 循环推流用
    virtual uint64_t totalUnits() const = 0;  // 已知总帧数（裸流=解析所得，mp4=sample_count）
};

// 写：裸流 / stdout / mp4。param sets 用于 mp4 首帧补齐（裸流忽略）。
class MediaWriter {
public:
    static std::unique_ptr<MediaWriter> create(const std::string& path, rtsp::CodecType codec,
                                               int width, int height, int fps,
                                               const std::vector<uint8_t>& vps,
                                               const std::vector<uint8_t>& sps,
                                               const std::vector<uint8_t>& pps,
                                               std::string& err);
    virtual ~MediaWriter() = default;
    virtual bool write(const rtsp::VideoFrame& frame) = 0;  // frame.data 为 Annex-B
    virtual bool finish() = 0;                               // 收尾（mp4 写 moov）
};

// 工具：判断扩展名是否 .mp4（大小写不敏感）。
bool isMp4Path(const std::string& path);

} // namespace rtspcli
