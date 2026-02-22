#pragma once

/**
 * @file rtsp_client.h
 * @brief RTSP Client Library - 客户端库
 * 
 * 提供 RTSP 客户端功能，支持从 RTSP 服务器拉流。
 * 支持 H.264/H.265 视频流，提供回调和阻塞两种方式接收帧数据。
 */

#include <rtsp-common/common.h>
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <queue>

namespace rtsp {

// 前向声明
class RtpReceiver;

/**
 * @brief RTSP客户端配置
 */
struct RtspClientConfig {
    std::string user_agent = "RtspClient/1.0";  ///< User-Agent字符串
    uint32_t rtp_port_start = 20000;             ///< RTP端口起始范围
    uint32_t rtp_port_end = 30000;               ///< RTP端口结束范围
    bool prefer_tcp_transport = false;           ///< 是否优先使用RTP over RTSP(TCP)
    bool fallback_to_tcp = true;                 ///< UDP失败时是否回退到TCP
    uint32_t jitter_buffer_packets = 32;         ///< RTP乱序重排窗口（包数）
    
    /**
     * @brief 帧缓冲区大小（最大缓存帧数）
     * 
     * 当使用 receiveFrame() 或设置了帧回调时，接收到的帧会先存入内部队列。
     * 如果队列满，最旧的帧会被丢弃。
     * 
     * 默认: 30帧（约1秒@30fps）
     * 建议: 根据应用场景调整，实时性要求高可以设小一些
     */
    uint32_t buffer_size = 30;
    
    /**
     * @brief 接收超时时间（毫秒）
     */
    uint32_t receive_timeout_ms = 5000;
};

/**
 * @brief 媒体信息
 */
struct MediaInfo {
    std::string control_url;    ///< 控制URL
    CodecType codec;            ///< 编码类型
    std::string codec_name;     ///< 编码名称
    uint32_t width;             ///< 视频宽度
    uint32_t height;            ///< 视频高度
    uint32_t fps;               ///< 帧率
    uint32_t payload_type;      ///< RTP payload type
    uint32_t clock_rate;        ///< 时钟频率（通常90000）
    std::vector<uint8_t> sps;   ///< SPS数据
    std::vector<uint8_t> pps;   ///< PPS数据
    std::vector<uint8_t> vps;   ///< VPS数据（仅HEVC）
};

/**
 * @brief 会话信息
 */
struct SessionInfo {
    std::string session_id;           ///< 会话ID
    std::string base_url;             ///< 基础URL
    std::vector<MediaInfo> media_streams;  ///< 媒体流列表
    uint64_t duration_ms;             ///< 时长（毫秒）
    bool has_video;                   ///< 是否有视频
    bool has_audio;                   ///< 是否有音频
};

struct RtspClientStats {
    uint64_t auth_retries = 0;
    uint64_t rtp_packets_received = 0;
    uint64_t rtp_packets_reordered = 0;
    uint64_t rtp_packet_loss_events = 0;
    uint64_t frames_output = 0;
    bool using_tcp_transport = false;
};

/**
 * @brief 帧回调函数类型
 * 
 * @param frame 视频帧数据，回调结束后内存会被释放
 * 
 * 注意：回调在内部线程中执行，不要阻塞太久
 */
using FrameCallback = std::function<void(const VideoFrame& frame)>;

/**
 * @brief 错误回调函数类型
 */
using ErrorCallback = std::function<void(const std::string& error)>;

/**
 * @brief RTSP客户端类
 * 
 * 使用示例（回调方式）：
 * @code
 * RtspClient client;
 * client.setFrameCallback([](const VideoFrame& frame) {
 *     // 处理帧
 * });
 * 
 * if (client.open("rtsp://example.com/stream")) {
 *     client.describe();
 *     client.setup(0);
 *     client.play(0);
 *     client.receiveLoop();  // 阻塞接收
 * }
 * @endcode
 * 
 * 使用示例（阻塞方式）：
 * @code
 * RtspClient client;
 * if (client.open("rtsp://example.com/stream")) {
 *     client.describe();
 *     client.setup(0);
 *     client.play(0);
 *     
 *     VideoFrame frame;
 *     while (client.receiveFrame(frame, 1000)) {
 *         // 处理帧
 *         delete[] frame.data;
 *     }
 * }
 * @endcode
 */
class RtspClient {
public:
    RtspClient();
    ~RtspClient();

    /**
     * @brief 设置配置
     */
    void setConfig(const RtspClientConfig& config);
    
    /**
     * @brief 连接到服务器
     * @param url RTSP URL, e.g., "rtsp://127.0.0.1:8554/live"
     * @return 是否成功
     */
    bool open(const std::string& url);
    
    /**
     * @brief 发送DESCRIBE请求获取流信息
     * @return 是否成功
     */
    bool describe();
    
    /**
     * @brief 获取会话信息
     */
    SessionInfo getSessionInfo() const;
    
    /**
     * @brief 设置帧回调
     * 
     * 设置后，接收到的帧会通过回调通知。
     * 回调在内部线程中执行，注意线程安全。
     * 
     * @param callback 帧回调函数
     */
    void setFrameCallback(FrameCallback callback);
    
    /**
     * @brief 设置错误回调
     */
    void setErrorCallback(ErrorCallback callback);
    
    /**
     * @brief 发送SETUP请求
     * @param stream_index 媒体流索引（从describe获取）
     * @return 是否成功
     */
    bool setup(int stream_index = 0);
    
    /**
     * @brief 发送PLAY请求开始播放
     * @param start_time_ms 起始时间（毫秒），0表示从头开始
     * @return 是否成功
     */
    bool play(uint64_t start_time_ms = 0);
    
    /**
     * @brief 发送PAUSE请求暂停
     * @return 是否成功
     */
    bool pause();
    
    /**
     * @brief 发送TEARDOWN请求结束
     * @return 是否成功
     */
    bool teardown();
    
    /**
     * @brief 接收循环（阻塞）
     * 
     * 内部维护接收线程，通过回调通知帧数据。
     * 调用 close() 或 teardown() 可退出循环。
     */
    void receiveLoop();
    
    /**
     * @brief 非阻塞接收单帧
     * 
     * 内部有队列缓存，队列大小由 RtspClientConfig::buffer_size 控制。
     * 队列满时会丢弃最旧的帧。
     * 
     * @param frame 输出帧数据（需调用者 delete[] frame.data）
     * @param timeout_ms 超时时间（毫秒）
     * @return true 成功获取一帧，false 超时或出错
     */
    bool receiveFrame(VideoFrame& frame, int timeout_ms = 1000);
    
    /**
     * @brief 是否已连接
     */
    bool isConnected() const;
    
    /**
     * @brief 是否正在播放
     */
    bool isPlaying() const;
    
    /**
     * @brief 关闭连接
     */
    void close();

    /**
     * @brief 发送OPTIONS请求
     */
    bool sendOptions();
    
    /**
     * @brief 发送GET_PARAMETER请求
     */
    bool sendGetParameter(const std::string& param);
    RtspClientStats getStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief 简化版RTSP播放器（封装了常用操作）
 * 
 * 支持两种方式获取帧数据：
 * 1. 回调方式：设置帧回调，在回调中处理帧（推荐）
 * 2. 阻塞方式：调用 readFrame() 阻塞读取
 * 
 * 内部维护接收线程，自动缓存帧数据。
 * 队列大小由 RtspClientConfig::buffer_size 控制。
 * 
 * 使用示例（回调方式）：
 * @code
 * SimpleRtspPlayer player;
 * player.setFrameCallback([](const VideoFrame& frame) {
 *     // 处理帧（内部线程调用，不要阻塞）
 * });
 * player.setErrorCallback([](const std::string& error) {
 *     std::cerr << "Error: " << error << std::endl;
 * });
 * 
 * if (player.open("rtsp://127.0.0.1:8554/live")) {
 *     // 播放器会自动在后台接收帧并通过回调通知
 *     // 主线程可以做其他事情
 *     std::this_thread::sleep_for(std::chrono::seconds(30));
 * }
 * player.close();
 * @endcode
 * 
 * 使用示例（阻塞方式）：
 * @code
 * SimpleRtspPlayer player;
 * if (player.open("rtsp://127.0.0.1:8554/live")) {
 *     VideoFrame frame;
 *     while (player.readFrame(frame)) {
 *         // 处理帧
 *         delete[] frame.data;
 *     }
 * }
 * player.close();
 * @endcode
 */
class SimpleRtspPlayer {
public:
    SimpleRtspPlayer();
    ~SimpleRtspPlayer();

    /**
     * @brief 设置帧回调
     * 
     * 设置后，接收到的帧会通过回调通知。
     * 回调在内部线程中执行，注意线程安全，不要阻塞太久。
     * 
     * @param callback 帧回调函数
     */
    void setFrameCallback(FrameCallback callback);
    
    /**
     * @brief 设置错误回调
     * 
     * 当发生错误时通过回调通知。
     * 
     * @param callback 错误回调函数
     */
    void setErrorCallback(ErrorCallback callback);

    /**
     * @brief 打开URL并开始播放
     * @param url RTSP URL
     * @return 是否成功
     */
    bool open(const std::string& url);
    
    /**
     * @brief 读取一帧（阻塞方式）
     * 
     * 如果没有设置回调，可以使用此方法阻塞读取帧。
     * 内部有队列缓存，队列满时会丢弃最旧的帧。
     * 
     * @param frame 输出帧数据（需调用者 delete[] frame.data）
     * @return true 成功，false 结束或出错
     */
    bool readFrame(VideoFrame& frame);
    
    /**
     * @brief 关闭播放器
     */
    void close();
    
    /**
     * @brief 获取媒体信息
     */
    bool getMediaInfo(uint32_t& width, uint32_t& height, uint32_t& fps, CodecType& codec);
    
    /**
     * @brief 检查是否正在运行
     */
    bool isRunning() const;

private:
    std::unique_ptr<RtspClient> client_;
    std::vector<VideoFrame> frame_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    std::thread receive_thread_;
    std::atomic<bool> running_{false};
    
    FrameCallback frame_callback_;
    ErrorCallback error_callback_;
    
    static constexpr size_t MAX_BUFFER_SIZE = 30;  ///< 默认最大缓存帧数
};

} // namespace rtsp
