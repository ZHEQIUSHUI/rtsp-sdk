#pragma once

#include <rtsp-common/common.h>
#include <string>
#include <memory>
#include <functional>

namespace rtsp {

// 前向声明
class MediaSession;

// RTSP服务器配置
struct RtspServerConfig {
    std::string host = "0.0.0.0";     // 监听地址
    uint16_t port = 554;               // 监听端口
    uint32_t session_timeout_ms = 60000;  // 会话超时时间
    uint32_t rtp_port_start = 10000;   // RTP端口起始
    uint32_t rtp_port_end = 20000;     // RTP端口结束
    uint32_t rtp_port_current = 10000; // 当前RTP端口
    bool auth_enabled = false;         // 是否启用Basic鉴权
    bool auth_use_digest = false;      // 是否使用Digest鉴权（否则Basic）
    std::string auth_username;         // 鉴权用户名
    std::string auth_password;         // 鉴权密码
    std::string auth_realm = "RTSP Server"; // 鉴权域
    std::string auth_nonce;            // Digest nonce（可选，空则自动生成）
    uint32_t auth_nonce_ttl_ms = 60000; // Digest nonce有效期
    
    static uint16_t getNextRtpPort(uint32_t& current, uint32_t start, uint32_t end);
};

struct RtspServerStats {
    uint64_t requests_total = 0;
    uint64_t auth_challenges = 0;
    uint64_t auth_failures = 0;
    uint64_t sessions_created = 0;
    uint64_t sessions_closed = 0;
    uint64_t frames_pushed = 0;
    uint64_t rtp_packets_sent = 0;
    uint64_t rtp_bytes_sent = 0;
};

// 媒体路径配置
struct PathConfig {
    std::string path;                  // 路径，如 "/live/stream1"
    CodecType codec = CodecType::H264; // 编码类型
    uint32_t width = 1920;             // 视频宽度
    uint32_t height = 1080;            // 视频高度
    uint32_t fps = 30;                 // 帧率
    std::vector<uint8_t> sps;          // SPS
    std::vector<uint8_t> pps;          // PPS
    std::vector<uint8_t> vps;          // VPS (仅HEVC)
};

// 视频帧输入接口
class IVideoFrameInput {
public:
    virtual ~IVideoFrameInput() = default;
    virtual bool pushFrame(const VideoFrame& frame) = 0;
};

// RTSP服务器类
class RtspServer {
public:
    RtspServer();
    ~RtspServer();

    // 初始化服务器
    bool init(const RtspServerConfig& config);
    bool init(const std::string& host, uint16_t port);
    
    // 启动/停止服务器
    bool start();
    void stop();
    bool isRunning() const;
    
    // 添加媒体路径
    bool addPath(const PathConfig& config);
    bool addPath(const std::string& path, CodecType codec);
    
    // 删除媒体路径
    bool removePath(const std::string& path);
    
    // 推送视频帧到指定路径（线程安全）
    bool pushFrame(const std::string& path, const VideoFrame& frame);
    
    // 推送H.264/H.265数据（原始NALU，带起始码）
    bool pushH264Data(const std::string& path, const uint8_t* data, size_t size,
                      uint64_t pts, bool is_key);
    bool pushH265Data(const std::string& path, const uint8_t* data, size_t size,
                      uint64_t pts, bool is_key);
    
    // 获取帧输入接口（用于更复杂的场景）
    std::shared_ptr<IVideoFrameInput> getFrameInput(const std::string& path);
    
    // 设置回调
    using ClientConnectCallback = std::function<void(const std::string& path, const std::string& client_ip)>;
    using ClientDisconnectCallback = std::function<void(const std::string& path, const std::string& client_ip)>;
    
    void setClientConnectCallback(ClientConnectCallback callback);
    void setClientDisconnectCallback(ClientDisconnectCallback callback);
    void setAuth(const std::string& username, const std::string& password,
                 const std::string& realm = "RTSP Server");
    void setAuthDigest(const std::string& username, const std::string& password,
                       const std::string& realm = "RTSP Server");
    RtspServerStats getStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 工具函数：创建视频帧（内部会复制数据并使用智能托管）
VideoFrame createVideoFrame(CodecType codec, const uint8_t* data, size_t size,
                            uint64_t pts, uint32_t width, uint32_t height, uint32_t fps);

// 工具函数：显式释放视频帧内存（可选）
void freeVideoFrame(VideoFrame& frame);

} // namespace rtsp
