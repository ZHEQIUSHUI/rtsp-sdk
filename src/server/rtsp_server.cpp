#include <rtsp-server/rtsp_server.h>
#include <rtsp-common/rtsp_request.h>
#include <rtsp-common/sdp.h>
#include <rtsp-common/rtp_packer.h>
#include <rtsp-common/socket.h>
#include <rtsp-common/common.h>

#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <cstring>
#include <condition_variable>
#include <queue>
#include <vector>
#include <regex>
#include <unordered_map>

namespace rtsp {

namespace {

inline bool hasStartCode3(const uint8_t* data, size_t size, size_t i) {
    return i + 3 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01;
}

inline bool hasStartCode4(const uint8_t* data, size_t size, size_t i) {
    return i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 &&
           data[i + 2] == 0x00 && data[i + 3] == 0x01;
}

template <typename Fn>
void forEachAnnexBNalu(const uint8_t* data, size_t size, Fn&& fn) {
    if (!data || size == 0) {
        return;
    }

    std::vector<size_t> starts;
    starts.reserve(16);
    for (size_t i = 0; i + 3 < size; ++i) {
        if (hasStartCode4(data, size, i)) {
            starts.push_back(i + 4);
            i += 3;
        } else if (hasStartCode3(data, size, i)) {
            starts.push_back(i + 3);
            i += 2;
        }
    }

    if (starts.empty()) {
        fn(data, size);
        return;
    }

    for (size_t idx = 0; idx < starts.size(); ++idx) {
        const size_t nalu_start = starts[idx];
        size_t nalu_end = (idx + 1 < starts.size()) ? starts[idx + 1] : size;
        if (nalu_end > nalu_start) {
            fn(data + nalu_start, nalu_end - nalu_start);
        }
    }
}

bool assignIfChanged(std::vector<uint8_t>& dst, const uint8_t* src, size_t src_size) {
    if (!src || src_size == 0) {
        return false;
    }
    if (dst.size() == src_size && std::memcmp(dst.data(), src, src_size) == 0) {
        return false;
    }
    dst.assign(src, src + src_size);
    return true;
}

bool autoExtractH264ParameterSets(PathConfig& config, const uint8_t* data, size_t size) {
    bool updated = false;
    forEachAnnexBNalu(data, size, [&](const uint8_t* nalu, size_t nalu_size) {
        if (nalu_size == 0) {
            return;
        }
        const uint8_t type = nalu[0] & 0x1F;
        if (type == 7) {
            updated = assignIfChanged(config.sps, nalu, nalu_size) || updated;
        } else if (type == 8) {
            updated = assignIfChanged(config.pps, nalu, nalu_size) || updated;
        }
    });
    return updated;
}

bool autoExtractH265ParameterSets(PathConfig& config, const uint8_t* data, size_t size) {
    bool updated = false;
    forEachAnnexBNalu(data, size, [&](const uint8_t* nalu, size_t nalu_size) {
        if (nalu_size < 2) {
            return;
        }
        const uint8_t type = (nalu[0] >> 1) & 0x3F;
        if (type == 32) {
            updated = assignIfChanged(config.vps, nalu, nalu_size) || updated;
        } else if (type == 33) {
            updated = assignIfChanged(config.sps, nalu, nalu_size) || updated;
        } else if (type == 34) {
            updated = assignIfChanged(config.pps, nalu, nalu_size) || updated;
        }
    });
    return updated;
}

std::shared_ptr<std::vector<uint8_t>> makeManagedBuffer(const uint8_t* data, size_t size) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    if (data && size > 0) {
        buf->assign(data, data + size);
    }
    return buf;
}

VideoFrame cloneFrameManaged(const VideoFrame& src) {
    VideoFrame copy = src;
    copy.managed_data = makeManagedBuffer(src.data, src.size);
    copy.data = copy.managed_data->empty() ? nullptr : copy.managed_data->data();
    copy.size = copy.managed_data->size();
    return copy;
}

} // namespace

// 从完整RTSP URL中提取路径部分
// 支持格式: rtsp://host:port/path 或 /path
static std::string extractPathFromUrl(const std::string& url) {
    if (url.empty()) return "/";
    
    // 如果已经是纯路径（以/开头且没有scheme），直接返回
    if (url[0] == '/' && url.find("://") == std::string::npos) {
        return url;
    }
    
    // 去除 scheme (rtsp://)
    std::string temp = url;
    size_t scheme_pos = temp.find("://");
    if (scheme_pos != std::string::npos) {
        temp = temp.substr(scheme_pos + 3);
    }
    
    // 去除 host:port，提取路径
    size_t path_pos = temp.find('/');
    if (path_pos != std::string::npos) {
        // 去除查询参数
        size_t query_pos = temp.find('?', path_pos);
        if (query_pos != std::string::npos) {
            return temp.substr(path_pos, query_pos - path_pos);
        }
        return temp.substr(path_pos);
    }
    
    return "/";
}

// 生成随机session ID
static std::string generateSessionId() {
    static std::atomic<uint32_t> counter{0};
    uint32_t val = counter++;
    auto now = std::chrono::steady_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    
    std::stringstream ss;
    ss << std::hex << (ts & 0xFFFFFFFF) << val;
    return ss.str();
}

static std::string generateNonce() {
    return generateSessionId();
}

static std::unordered_map<std::string, std::string> parseAuthParams(const std::string& header_value) {
    std::unordered_map<std::string, std::string> kv;
    size_t pos = 0;
    while (pos < header_value.size()) {
        while (pos < header_value.size() && (header_value[pos] == ' ' || header_value[pos] == ',')) pos++;
        size_t eq = header_value.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = header_value.substr(pos, eq - pos);
        pos = eq + 1;
        std::string val;
        if (pos < header_value.size() && header_value[pos] == '"') {
            size_t endq = header_value.find('"', pos + 1);
            if (endq == std::string::npos) break;
            val = header_value.substr(pos + 1, endq - pos - 1);
            pos = endq + 1;
        } else {
            size_t comma = header_value.find(',', pos);
            if (comma == std::string::npos) {
                val = header_value.substr(pos);
                pos = header_value.size();
            } else {
                val = header_value.substr(pos, comma - pos);
                pos = comma + 1;
            }
        }
        kv[key] = val;
    }
    return kv;
}

struct ServerStatsAtomic {
    std::atomic<uint64_t> requests_total{0};
    std::atomic<uint64_t> auth_challenges{0};
    std::atomic<uint64_t> auth_failures{0};
    std::atomic<uint64_t> sessions_created{0};
    std::atomic<uint64_t> sessions_closed{0};
    std::atomic<uint64_t> frames_pushed{0};
    std::atomic<uint64_t> rtp_packets_sent{0};
    std::atomic<uint64_t> rtp_bytes_sent{0};
};

// 客户端会话
struct ClientSession {
    std::string session_id;
    std::string path;
    std::string client_ip;
    uint16_t client_rtp_port = 0;
    uint16_t client_rtcp_port = 0;
    
    std::unique_ptr<RtpSender> rtp_sender;
    std::unique_ptr<RtpPacker> rtp_packer;
    bool use_tcp_interleaved = false;
    uint8_t interleaved_rtp_channel = 0;
    std::shared_ptr<Socket> control_socket;
    std::shared_ptr<std::mutex> control_send_mutex;
    
    std::atomic<bool> playing{false};
    std::thread send_thread;
    
    // 帧队列
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::queue<VideoFrame> frame_queue;
    static constexpr size_t MAX_QUEUE_SIZE = 30;
    
    std::atomic<uint32_t> packet_count{0};
    std::atomic<uint32_t> octet_count{0};
    
    std::chrono::steady_clock::time_point last_activity;
    ServerStatsAtomic* stats = nullptr;
    
    ClientSession() {
        last_activity = std::chrono::steady_clock::now();
    }
    
    ~ClientSession() {
        stop();
    }
    
    void stop() {
        playing = false;
        queue_cv.notify_all();
        if (send_thread.joinable()) {
            send_thread.join();
        }
        
        // 清理队列
        std::lock_guard<std::mutex> lock(queue_mutex);
        while (!frame_queue.empty()) {
            auto& frame = frame_queue.front();
            freeVideoFrame(frame);
            frame_queue.pop();
        }
    }
    
    bool pushFrame(const VideoFrame& frame) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (frame_queue.size() >= MAX_QUEUE_SIZE) {
            // 队列满，丢弃最旧的帧
            auto& old = frame_queue.front();
            freeVideoFrame(old);
            frame_queue.pop();
        }
        
        // 复制帧
        VideoFrame copy = cloneFrameManaged(frame);
        
        frame_queue.push(copy);
        queue_cv.notify_one();
        return true;
    }
    
    void sendLoop() {
        while (playing) {
            VideoFrame frame;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [this] { return !frame_queue.empty() || !playing; });
                
                if (!playing) break;
                if (frame_queue.empty()) continue;
                
                frame = frame_queue.front();
                frame_queue.pop();
            }
            
            // 打包并发送
            if (rtp_packer && (use_tcp_interleaved || rtp_sender)) {
                auto packets = rtp_packer->packFrame(frame);
                
                for (const auto& packet : packets) {
                    if (use_tcp_interleaved) {
                        if (control_socket && control_socket->isValid() && control_send_mutex) {
                            std::vector<uint8_t> interleaved(4 + packet.size);
                            interleaved[0] = '$';
                            interleaved[1] = interleaved_rtp_channel;
                            interleaved[2] = static_cast<uint8_t>((packet.size >> 8) & 0xFF);
                            interleaved[3] = static_cast<uint8_t>(packet.size & 0xFF);
                            memcpy(interleaved.data() + 4, packet.data, packet.size);
                            std::lock_guard<std::mutex> sock_lock(*control_send_mutex);
                            control_socket->send(interleaved.data(), interleaved.size());
                        }
                    } else {
                        rtp_sender->sendRtpPacket(packet);
                    }
                    packet_count++;
                    octet_count += packet.size;
                    if (stats) {
                        stats->rtp_packets_sent++;
                        stats->rtp_bytes_sent += packet.size;
                    }
                    delete[] packet.data;
                }
                last_activity = std::chrono::steady_clock::now();
                
                // 定期发送RTCP SR
                if (packet_count % 100 == 0) {
                    auto now = std::chrono::system_clock::now();
                    auto epoch = now.time_since_epoch();
                    uint64_t ntp_ts = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
                    ntp_ts = (ntp_ts + 2208988800u) << 32;  // NTP epoch offset
                    
                    uint32_t rtp_ts = convertToRtpTimestamp(frame.pts, 90000);
                    rtp_sender->sendSenderReport(rtp_ts, ntp_ts, packet_count.load(), octet_count.load());
                }
            }
            
            freeVideoFrame(frame);
        }
    }
};

// 媒体路径
struct MediaPath {
    std::string path;
    PathConfig config;
    
    std::mutex sessions_mutex;
    std::map<std::string, std::shared_ptr<ClientSession>> sessions;
    
    // 最新帧（用于新连接的客户端）
    std::mutex latest_frame_mutex;
    VideoFrame latest_frame;
    bool has_latest_frame = false;
    
    void broadcastFrame(const VideoFrame& frame) {
        // 更新最新帧
        {
            std::lock_guard<std::mutex> lock(latest_frame_mutex);
            freeVideoFrame(latest_frame);
            latest_frame = cloneFrameManaged(frame);
            has_latest_frame = true;
        }
        
        // 广播到所有客户端
        std::lock_guard<std::mutex> lock(sessions_mutex);
        for (auto& session_pair : sessions) {
            auto& session = session_pair.second;
            if (session->playing) {
                session->pushFrame(frame);
            }
        }
    }
    
    void addSession(const std::string& session_id, std::shared_ptr<ClientSession> session) {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        sessions[session_id] = session;
        
        // 发送最新帧给新客户端（关键帧优先）
        std::lock_guard<std::mutex> lf_lock(latest_frame_mutex);
        if (has_latest_frame && latest_frame.type == FrameType::IDR) {
            session->pushFrame(latest_frame);
        }
    }
    
    void removeSession(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        auto it = sessions.find(session_id);
        if (it != sessions.end()) {
            it->second->stop();
            sessions.erase(it);
        }
    }
    
    ~MediaPath() {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        for (auto& session_pair : sessions) {
            auto& session = session_pair.second;
            session->stop();
        }
        sessions.clear();
        
        freeVideoFrame(latest_frame);
    }
};

// RTSP连接处理
class RtspConnection {
public:
    RtspConnection(std::shared_ptr<Socket> socket,
                   std::map<std::string, std::shared_ptr<MediaPath>>& paths,
                   std::mutex& paths_mutex,
                   RtspServerConfig& config,
                   RtspServer::ClientConnectCallback& connect_cb,
                   RtspServer::ClientDisconnectCallback& disconnect_cb,
                   ServerStatsAtomic& stats)
        : socket_(std::move(socket)),
          paths_(paths),
          paths_mutex_(paths_mutex),
          config_(config),
          connect_cb_(connect_cb),
          disconnect_cb_(disconnect_cb),
          send_mutex_(std::make_shared<std::mutex>()),
          stats_(stats),
          digest_nonce_(config.auth_nonce.empty() ? "nonce-" + generateNonce() : config.auth_nonce),
          digest_nonce_created_(std::chrono::steady_clock::now()) {}
    
    void handle() {
        std::string buffer;
        buffer.reserve(4096);
        
        uint8_t temp[4096];
        while (socket_->isValid()) {
            ssize_t n = socket_->recv(temp, sizeof(temp), 1000);
            if (n > 0) {
                buffer.append((char*)temp, n);
                
                // 检查是否收到完整请求
                size_t pos;
                while ((pos = buffer.find("\r\n\r\n")) != std::string::npos) {
                    size_t content_length = 0;
                    size_t header_end = pos + 4;
                    
                    // 解析Content-Length
                    std::string header = buffer.substr(0, header_end);
                    std::string header_lower = header;
                    std::transform(header_lower.begin(), header_lower.end(), header_lower.begin(), ::tolower);
                    size_t cl_pos = header_lower.find("content-length:");
                    if (cl_pos != std::string::npos) {
                        size_t cl_end = header_lower.find("\r\n", cl_pos);
                        if (cl_end != std::string::npos) {
                            std::string cl_str = header.substr(cl_pos + 15, cl_end - cl_pos - 15);
                            try {
                                content_length = static_cast<size_t>(std::stoul(cl_str));
                            } catch (...) {
                                // 非法Content-Length，丢弃当前缓冲避免崩溃
                                buffer.clear();
                                break;
                            }
                        }
                    }
                    
                    // 检查是否有足够的主体数据
                    if (buffer.size() < header_end + content_length) {
                        break;
                    }
                    
                    // 处理请求
                    std::string request_data = buffer.substr(0, header_end + content_length);
                    buffer.erase(0, header_end + content_length);
                    
                    processRequest(request_data);
                }
            } else if (n == 0) {
                // 连接关闭
                break;
            }
            // n < 0 是超时，继续循环
        }
        
        // 清理会话
        if (session_) {
            auto path_it = paths_.find(session_->path);
            if (path_it != paths_.end()) {
                path_it->second->removeSession(session_->session_id);
            }
            if (disconnect_cb_) {
                disconnect_cb_(session_->path, session_->client_ip);
            }
        }
    }

private:
    void processRequest(const std::string& data) {
        RtspRequest request;
        if (!request.parse(data)) {
            return;
        }
        stats_.requests_total++;
        
        int cseq = request.getCSeq();
        if (!checkAuthorization(request, cseq)) {
            return;
        }
        if (session_) {
            session_->last_activity = std::chrono::steady_clock::now();
        }
        
        RTSP_LOG_INFO("RTSP " + RtspRequest::methodToString(request.getMethod()) + 
                      " " + request.getUri());
        
        switch (request.getMethod()) {
            case RtspMethod::Options:
                sendResponse(RtspResponse::createOptions(cseq));
                break;
                
            case RtspMethod::Describe:
                handleDescribe(request, cseq);
                break;
                
            case RtspMethod::Setup:
                handleSetup(request, cseq);
                break;
                
            case RtspMethod::Play:
                handlePlay(request, cseq);
                break;

            case RtspMethod::Pause:
                handlePause(request, cseq);
                break;

            case RtspMethod::GetParameter:
                handleGetParameter(request, cseq);
                break;

            case RtspMethod::SetParameter:
                handleSetParameter(request, cseq);
                break;
                
            case RtspMethod::Teardown:
                handleTeardown(request, cseq);
                break;
                
            default:
                sendResponse(RtspResponse::createError(cseq, 501, "Not Implemented"));
                break;
        }
    }
    
    void handleDescribe(const RtspRequest& request, int cseq) {
        std::string path = extractPathFromUrl(request.getPath());
        
        std::lock_guard<std::mutex> lock(paths_mutex_);
        auto it = paths_.find(path);
        if (it == paths_.end()) {
            sendResponse(RtspResponse::createError(cseq, 404, "Not Found"));
            return;
        }
        
        auto& media_path = it->second;
        auto& config = media_path->config;
        
        // 构建SDP
        SdpBuilder sdp;
        // 使用 0.0.0.0 表示接受任何地址的连接
        sdp.setConnection("IN", "IP4", "0.0.0.0");
        
        // 计算payload type
        uint8_t payload_type = (config.codec == CodecType::H264) ? 96 : 97;
        uint32_t clock_rate = 90000;
        
        std::string sps_b64 = base64Encode(config.sps.data(), config.sps.size());
        std::string pps_b64 = base64Encode(config.pps.data(), config.pps.size());
        std::string vps_b64 = base64Encode(config.vps.data(), config.vps.size());
        
        std::string control = "stream";
        
        if (config.codec == CodecType::H264) {
            sdp.addH264Media(control, 0, payload_type, clock_rate,
                            sps_b64, pps_b64, config.width, config.height);
        } else {
            sdp.addH265Media(control, 0, payload_type, clock_rate,
                            vps_b64, sps_b64, pps_b64, config.width, config.height);
        }
        
        sendResponse(RtspResponse::createDescribe(cseq, sdp.build()));
    }
    
    void handleSetup(const RtspRequest& request, int cseq) {
        if (session_) {
            sendResponse(RtspResponse::createError(cseq, 459, "Aggregate Operation Not Allowed"));
            return;
        }

        std::string path = extractPathFromUrl(request.getPath());
        
        // 从path中提取主路径
        size_t slash_pos = path.find_last_of('/');
        if (slash_pos != std::string::npos) {
            path = path.substr(0, slash_pos);
        }
        
        std::lock_guard<std::mutex> lock(paths_mutex_);
        auto it = paths_.find(path);
        if (it == paths_.end()) {
            sendResponse(RtspResponse::createError(cseq, 404, "Not Found"));
            return;
        }
        
        auto& media_path = it->second;
        
        // 获取客户端端口
        std::string transport = request.getTransport();
        
        // 检查是否是 TCP 传输 (RTP/AVP/TCP)
        bool use_tcp = (transport.find("RTP/AVP/TCP") != std::string::npos ||
                        transport.find("TCP") != std::string::npos);
        
        int client_rtp_port = request.getRtpPort();
        int client_rtcp_port = request.getRtcpPort();
        
        // UDP 模式需要客户端端口，TCP 模式不需要
        if (client_rtp_port == 0 && !use_tcp) {
            sendResponse(RtspResponse::createError(cseq, 400, "Bad Request"));
            return;
        }
        
        // 创建会话
        session_ = std::make_shared<ClientSession>();
        session_->session_id = generateSessionId();
        session_->path = path;
        session_->client_ip = socket_->getPeerIp();
        session_->client_rtp_port = static_cast<uint16_t>(client_rtp_port);
        session_->client_rtcp_port = static_cast<uint16_t>(client_rtcp_port != 0 ? client_rtcp_port : client_rtp_port + 1);
        session_->use_tcp_interleaved = use_tcp;
        session_->control_socket = socket_;
        session_->control_send_mutex = send_mutex_;
        session_->stats = &stats_;
        
        // 创建RTP发送器
        if (!use_tcp) {
            session_->rtp_sender = std::make_unique<RtpSender>();
            uint16_t local_rtp_port = RtspServerConfig::getNextRtpPort(
                config_.rtp_port_current, config_.rtp_port_start, config_.rtp_port_end);
            
            if (!session_->rtp_sender->init("0.0.0.0", local_rtp_port)) {
                sendResponse(RtspResponse::createError(cseq, 500, "Internal Server Error"));
                session_.reset();
                return;
            }
            
            session_->rtp_sender->setPeer(session_->client_ip, 
                                           session_->client_rtp_port, 
                                           session_->client_rtcp_port);
        }
        
        // 创建RTP打包器
        if (media_path->config.codec == CodecType::H264) {
            session_->rtp_packer = std::make_unique<H264RtpPacker>();
        } else {
            session_->rtp_packer = std::make_unique<H265RtpPacker>();
        }
        session_->rtp_packer->setPayloadType((media_path->config.codec == CodecType::H264) ? 96 : 97);
        session_->rtp_packer->setSsrc(0x12345678 + std::hash<std::string>{}(session_->session_id));
        
        // 添加到媒体路径
        media_path->addSession(session_->session_id, session_);
        stats_.sessions_created++;
        if (connect_cb_) {
            connect_cb_(session_->path, session_->client_ip);
        }
        
        // 重新计算 use_tcp（因为前面代码块中的变量作用域问题）
        bool is_tcp = use_tcp;
        if (is_tcp) {
            std::regex ch_re("interleaved=(\\d+)-(\\d+)", std::regex::icase);
            std::smatch m;
            if (std::regex_search(transport, m, ch_re)) {
                session_->interleaved_rtp_channel = static_cast<uint8_t>(std::stoi(m[1].str()));
            }
        }
        
        // 构建transport响应
        std::stringstream transport_ss;
        if (is_tcp) {
            // TCP 模式 (RTP over RTSP)
            transport_ss << "RTP/AVP/TCP;unicast;interleaved="
                         << static_cast<int>(session_->interleaved_rtp_channel) << "-"
                         << static_cast<int>(session_->interleaved_rtp_channel + 1);
        } else {
            // UDP 模式
            transport_ss << "RTP/AVP;unicast;client_port=" << client_rtp_port 
                         << "-" << session_->client_rtcp_port
                         << ";server_port=" << session_->rtp_sender->getLocalPort()
                         << "-" << session_->rtp_sender->getLocalRtcpPort();
        }
        
        sendResponse(RtspResponse::createSetup(cseq, session_->session_id, transport_ss.str()));
    }
    
    void handlePlay(const RtspRequest& request, int cseq) {
        if (!session_) {
            sendResponse(RtspResponse::createError(cseq, 455, "Method Not Valid In This State"));
            return;
        }
        
        std::string session_id = request.getSession();
        if (session_id != session_->session_id) {
            sendResponse(RtspResponse::createError(cseq, 454, "Session Not Found"));
            return;
        }

        // 幂等处理：已在播放直接返回成功，避免重复创建线程导致崩溃
        if (!session_->playing) {
            session_->playing = true;
            if (!session_->send_thread.joinable()) {
                session_->send_thread = std::thread([this]() {
                    session_->sendLoop();
                });
            }
        }
        
        sendResponse(RtspResponse::createPlay(cseq, session_->session_id));
    }

    void handlePause(const RtspRequest& request, int cseq) {
        (void)request;
        if (!session_) {
            sendResponse(RtspResponse::createError(cseq, 455, "Method Not Valid In This State"));
            return;
        }
        session_->stop();
        sendResponse(RtspResponse::createOk(cseq));
    }

    void handleGetParameter(const RtspRequest& request, int cseq) {
        // Keep-alive support: session存在则返回200，否则返回454
        std::string sid = request.getSession();
        if (!session_ || (!sid.empty() && sid != session_->session_id)) {
            sendResponse(RtspResponse::createError(cseq, 454, "Session Not Found"));
            return;
        }
        sendResponse(RtspResponse::createOk(cseq));
    }

    void handleSetParameter(const RtspRequest& request, int cseq) {
        // Keep-alive / parameter set: 当前实现接受请求但不持久化参数
        std::string sid = request.getSession();
        if (!session_ || (!sid.empty() && sid != session_->session_id)) {
            sendResponse(RtspResponse::createError(cseq, 454, "Session Not Found"));
            return;
        }
        sendResponse(RtspResponse::createOk(cseq));
    }
    
    void handleTeardown(const RtspRequest& request, int cseq) {
        (void)request;
        if (session_) {
            auto path_it = paths_.find(session_->path);
            if (path_it != paths_.end()) {
                path_it->second->removeSession(session_->session_id);
            }
            stats_.sessions_closed++;
            if (disconnect_cb_) {
                disconnect_cb_(session_->path, session_->client_ip);
            }
            session_.reset();
        }
        
        sendResponse(RtspResponse::createTeardown(cseq));
    }
    
    void sendResponse(const RtspResponse& response) {
        std::string data = response.build();
        std::lock_guard<std::mutex> lock(*send_mutex_);
        socket_->send((const uint8_t*)data.c_str(), data.size());
    }

    bool checkAuthorization(const RtspRequest& request, int cseq) {
        if (!config_.auth_enabled) return true;
        if (request.getMethod() == RtspMethod::Options) return true;

        std::string auth = request.getHeader("Authorization");
        auto reject = [&](bool digest, bool stale) {
            RtspResponse resp(cseq);
            resp.setStatus(401, "Unauthorized");
            stats_.auth_challenges++;
            stats_.auth_failures++;
            if (digest) {
                resp.setHeader("WWW-Authenticate",
                               "Digest realm=\"" + config_.auth_realm +
                               "\", nonce=\"" + digest_nonce_ +
                               "\", algorithm=MD5, qop=\"auth\"" +
                               std::string(stale ? ", stale=true" : ""));
            } else {
                resp.setHeader("WWW-Authenticate", "Basic realm=\"" + config_.auth_realm + "\"");
            }
            sendResponse(resp);
            return false;
        };

        if (config_.auth_use_digest) {
            auto now = std::chrono::steady_clock::now();
            if (now - digest_nonce_created_ > std::chrono::milliseconds(config_.auth_nonce_ttl_ms)) {
                digest_nonce_ = "nonce-" + generateNonce();
                digest_nonce_created_ = now;
                digest_nc_seen_.clear();
                return reject(true, true);
            }
            if (auth.rfind("Digest ", 0) != 0) {
                return reject(true, false);
            }
            auto params = parseAuthParams(auth.substr(7));
            const std::string username = params["username"];
            const std::string realm = params["realm"];
            const std::string header_nonce = params["nonce"];
            const std::string uri = params["uri"];
            const std::string response = params["response"];
            const std::string qop = params["qop"];
            const std::string nc = params["nc"];
            const std::string cnonce = params["cnonce"];

            if (username.empty() || realm.empty() || header_nonce.empty() || uri.empty() || response.empty()) {
                return reject(true, false);
            }
            if (username != config_.auth_username || realm != config_.auth_realm || header_nonce != digest_nonce_) {
                return reject(true, false);
            }

            std::string ha1 = md5Hex(config_.auth_username + ":" + config_.auth_realm + ":" + config_.auth_password);
            std::string ha2 = md5Hex(RtspRequest::methodToString(request.getMethod()) + ":" + uri);
            std::string expected;
            if (!qop.empty()) {
                if (nc.empty() || cnonce.empty()) {
                    return reject(true, false);
                }
                uint64_t nc_value = 0;
                try {
                    nc_value = std::stoull(nc, nullptr, 16);
                } catch (...) {
                    return reject(true, false);
                }
                std::string nc_key = username + "|" + cnonce + "|" + header_nonce;
                auto it = digest_nc_seen_.find(nc_key);
                if (it != digest_nc_seen_.end() && nc_value <= it->second) {
                    return reject(true, false);
                }
                digest_nc_seen_[nc_key] = nc_value;
                expected = md5Hex(ha1 + ":" + header_nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
            } else {
                expected = md5Hex(ha1 + ":" + header_nonce + ":" + ha2);
            }
            if (expected != response) {
                return reject(true, false);
            }
            return true;
        }

        if (auth.rfind("Basic ", 0) != 0) {
            return reject(false, false);
        }
        std::string encoded = auth.substr(6);
        auto decoded = base64Decode(encoded);
        std::string userpass(decoded.begin(), decoded.end());
        std::string expected = config_.auth_username + ":" + config_.auth_password;
        if (userpass != expected) {
            return reject(false, false);
        }
        return true;
    }

private:
    std::shared_ptr<Socket> socket_;
    std::map<std::string, std::shared_ptr<MediaPath>>& paths_;
    std::mutex& paths_mutex_;
    RtspServerConfig& config_;
    RtspServer::ClientConnectCallback& connect_cb_;
    RtspServer::ClientDisconnectCallback& disconnect_cb_;
    std::shared_ptr<std::mutex> send_mutex_;
    ServerStatsAtomic& stats_;
    std::shared_ptr<ClientSession> session_;
    std::string digest_nonce_;
    std::chrono::steady_clock::time_point digest_nonce_created_;
    std::unordered_map<std::string, uint64_t> digest_nc_seen_;
};

// RtspServer实现
class RtspServer::Impl {
public:
    RtspServerConfig config_;
    std::atomic<bool> running_{false};
    std::unique_ptr<TcpServer> tcp_server_;
    std::thread server_thread_;
    
    std::mutex paths_mutex_;
    std::map<std::string, std::shared_ptr<MediaPath>> paths_;
    
    ClientConnectCallback connect_callback_;
    ClientDisconnectCallback disconnect_callback_;
    ServerStatsAtomic stats_;

    std::mutex connections_mutex_;
    struct ConnectionHandle {
        std::shared_ptr<Socket> socket;
        std::thread thread;
    };
    std::vector<ConnectionHandle> connections_;
    
    std::thread cleanup_thread_;
    
    void cleanupLoop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            // 清理超时会话
            std::vector<std::pair<std::string, std::string>> disconnects;
            {
                std::lock_guard<std::mutex> lock(paths_mutex_);
                auto now = std::chrono::steady_clock::now();
                for (auto& path_pair : paths_) {
                    auto& path = path_pair.second;
                    std::lock_guard<std::mutex> session_lock(path->sessions_mutex);
                    for (auto it = path->sessions.begin(); it != path->sessions.end();) {
                        auto last_activity = it->second->last_activity;
                        if (now - last_activity > std::chrono::milliseconds(config_.session_timeout_ms)) {
                            RTSP_LOG_INFO("Session timeout: " + it->first);
                            it->second->stop();
                            stats_.sessions_closed++;
                            disconnects.emplace_back(path->path, it->second->client_ip);
                            it = path->sessions.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }
            for (const auto& d : disconnects) {
                if (disconnect_callback_) {
                    disconnect_callback_(d.first, d.second);
                }
            }
        }
    }
};

uint16_t RtspServerConfig::getNextRtpPort(uint32_t& current, uint32_t start, uint32_t end) {
    uint16_t port = (uint16_t)current;
    current += 2;
    if (current >= end) {
        current = start;
    }
    return port;
}

RtspServer::RtspServer() : impl_(std::make_unique<Impl>()) {}
RtspServer::~RtspServer() {
    stop();
}

bool RtspServer::init(const RtspServerConfig& config) {
    impl_->config_ = config;
    return true;
}

bool RtspServer::init(const std::string& host, uint16_t port) {
    impl_->config_.host = host;
    impl_->config_.port = port;
    return true;
}

bool RtspServer::start() {
    if (impl_->running_) return false;
    
    impl_->tcp_server_ = std::make_unique<TcpServer>();
    
    impl_->tcp_server_->setNewConnectionCallback([this](std::unique_ptr<Socket> socket) {
        auto shared_socket = std::shared_ptr<Socket>(std::move(socket));
        std::thread conn_thread([this, s = shared_socket]() mutable {
            RtspConnection conn(s, impl_->paths_, impl_->paths_mutex_, impl_->config_,
                                impl_->connect_callback_, impl_->disconnect_callback_,
                                impl_->stats_);
            conn.handle();
        });
        {
            std::lock_guard<std::mutex> lock(impl_->connections_mutex_);
            impl_->connections_.push_back({shared_socket, std::move(conn_thread)});
        }
    });
    
    if (!impl_->tcp_server_->start(impl_->config_.host, impl_->config_.port)) {
        RTSP_LOG_ERROR("Failed to start RTSP server on " + impl_->config_.host + 
                       ":" + std::to_string(impl_->config_.port));
        return false;
    }
    
    impl_->running_ = true;
    impl_->cleanup_thread_ = std::thread([this]() {
        impl_->cleanupLoop();
    });
    
    RTSP_LOG_INFO("RTSP server started on " + impl_->config_.host + 
                  ":" + std::to_string(impl_->config_.port));
    
    return true;
}

void RtspServer::stop() {
    impl_->running_ = false;
    
    if (impl_->tcp_server_) {
        impl_->tcp_server_->stop();
    }
    
    {
        std::lock_guard<std::mutex> lock(impl_->connections_mutex_);
        for (auto& c : impl_->connections_) {
            if (c.socket) {
                c.socket->close();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->connections_mutex_);
        for (auto& c : impl_->connections_) {
            if (c.thread.joinable()) {
                c.thread.join();
            }
        }
        impl_->connections_.clear();
    }
    
    if (impl_->cleanup_thread_.joinable()) {
        impl_->cleanup_thread_.join();
    }
    
    // 清理所有路径
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    impl_->paths_.clear();
}

bool RtspServer::isRunning() const {
    return impl_->running_;
}

bool RtspServer::addPath(const PathConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    
    if (impl_->paths_.find(config.path) != impl_->paths_.end()) {
        return false;
    }
    
    auto path = std::make_shared<MediaPath>();
    path->path = config.path;
    path->config = config;
    
    impl_->paths_[config.path] = path;
    
    RTSP_LOG_INFO("Added path: " + config.path);
    return true;
}

bool RtspServer::addPath(const std::string& path, CodecType codec) {
    PathConfig config;
    config.path = path;
    config.codec = codec;
    return addPath(config);
}

bool RtspServer::removePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    return impl_->paths_.erase(path) > 0;
}

bool RtspServer::pushFrame(const std::string& path, const VideoFrame& frame) {
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    
    auto it = impl_->paths_.find(path);
    if (it == impl_->paths_.end()) {
        return false;
    }
    
    it->second->broadcastFrame(frame);
    impl_->stats_.frames_pushed++;
    return true;
}

bool RtspServer::pushH264Data(const std::string& path, const uint8_t* data, size_t size,
                               uint64_t pts, bool is_key) {
    VideoFrame frame = {};
    frame.codec = CodecType::H264;
    frame.type = is_key ? FrameType::IDR : FrameType::P;
    frame.data = const_cast<uint8_t*>(data);
    frame.size = size;
    frame.pts = pts;
    frame.dts = pts;
    
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    auto it = impl_->paths_.find(path);
    if (it == impl_->paths_.end()) {
        return false;
    }

    bool updated = false;
    if (is_key || it->second->config.sps.empty() || it->second->config.pps.empty()) {
        updated = autoExtractH264ParameterSets(it->second->config, data, size);
    }
    if (updated) {
        RTSP_LOG_INFO("Auto-updated H264 parameter sets for path: " + path);
    }

    it->second->broadcastFrame(frame);
    impl_->stats_.frames_pushed++;
    return true;
}

bool RtspServer::pushH265Data(const std::string& path, const uint8_t* data, size_t size,
                               uint64_t pts, bool is_key) {
    VideoFrame frame = {};
    frame.codec = CodecType::H265;
    frame.type = is_key ? FrameType::IDR : FrameType::P;
    frame.data = const_cast<uint8_t*>(data);
    frame.size = size;
    frame.pts = pts;
    frame.dts = pts;
    
    std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
    auto it = impl_->paths_.find(path);
    if (it == impl_->paths_.end()) {
        return false;
    }

    bool updated = false;
    if (is_key || it->second->config.vps.empty() || it->second->config.sps.empty() || it->second->config.pps.empty()) {
        updated = autoExtractH265ParameterSets(it->second->config, data, size);
    }
    if (updated) {
        RTSP_LOG_INFO("Auto-updated H265 parameter sets for path: " + path);
    }

    it->second->broadcastFrame(frame);
    impl_->stats_.frames_pushed++;
    return true;
}

std::shared_ptr<IVideoFrameInput> RtspServer::getFrameInput(const std::string& path) {
    class FrameInput : public IVideoFrameInput {
    public:
        FrameInput(RtspServer::Impl* impl, std::string p) : impl_(impl), path_(std::move(p)) {}
        bool pushFrame(const VideoFrame& frame) override {
            std::lock_guard<std::mutex> lock(impl_->paths_mutex_);
            auto it = impl_->paths_.find(path_);
            if (it == impl_->paths_.end()) {
                return false;
            }
            it->second->broadcastFrame(frame);
            return true;
        }
    private:
        RtspServer::Impl* impl_;
        std::string path_;
    };
    return std::make_shared<FrameInput>(impl_.get(), path);
}

void RtspServer::setClientConnectCallback(ClientConnectCallback callback) {
    impl_->connect_callback_ = callback;
}

void RtspServer::setClientDisconnectCallback(ClientDisconnectCallback callback) {
    impl_->disconnect_callback_ = callback;
}

void RtspServer::setAuth(const std::string& username, const std::string& password,
                         const std::string& realm) {
    impl_->config_.auth_enabled = true;
    impl_->config_.auth_use_digest = false;
    impl_->config_.auth_username = username;
    impl_->config_.auth_password = password;
    impl_->config_.auth_realm = realm;
    if (impl_->config_.auth_nonce.empty()) {
        impl_->config_.auth_nonce = "nonce-" + generateNonce();
    }
}

void RtspServer::setAuthDigest(const std::string& username, const std::string& password,
                               const std::string& realm) {
    impl_->config_.auth_enabled = true;
    impl_->config_.auth_use_digest = true;
    impl_->config_.auth_username = username;
    impl_->config_.auth_password = password;
    impl_->config_.auth_realm = realm;
    if (impl_->config_.auth_nonce.empty()) {
        impl_->config_.auth_nonce = "nonce-" + generateNonce();
    }
}

RtspServerStats RtspServer::getStats() const {
    RtspServerStats s;
    s.requests_total = impl_->stats_.requests_total.load();
    s.auth_challenges = impl_->stats_.auth_challenges.load();
    s.auth_failures = impl_->stats_.auth_failures.load();
    s.sessions_created = impl_->stats_.sessions_created.load();
    s.sessions_closed = impl_->stats_.sessions_closed.load();
    s.frames_pushed = impl_->stats_.frames_pushed.load();
    s.rtp_packets_sent = impl_->stats_.rtp_packets_sent.load();
    s.rtp_bytes_sent = impl_->stats_.rtp_bytes_sent.load();
    return s;
}

// 工具函数
VideoFrame createVideoFrame(CodecType codec, const uint8_t* data, size_t size,
                            uint64_t pts, uint32_t width, uint32_t height, uint32_t fps) {
    VideoFrame frame = {};
    frame.codec = codec;
    frame.type = FrameType::P;
    frame.managed_data = makeManagedBuffer(data, size);
    frame.data = frame.managed_data->empty() ? nullptr : frame.managed_data->data();
    frame.size = frame.managed_data->size();
    frame.pts = pts;
    frame.dts = pts;
    frame.width = width;
    frame.height = height;
    frame.fps = fps;
    return frame;
}

void freeVideoFrame(VideoFrame& frame) {
    if (!frame.managed_data && frame.data) {
        delete[] frame.data;
    }
    frame.managed_data.reset();
    frame.data = nullptr;
    frame.size = 0;
}

} // namespace rtsp
