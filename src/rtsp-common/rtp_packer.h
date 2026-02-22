#pragma once

#include <rtsp-common/common.h>
#include <vector>

namespace rtsp {

// RTP打包器基类
class RtpPacker {
public:
    virtual ~RtpPacker() = default;
    
    // 设置SSRC和初始序列号
    void setSsrc(uint32_t ssrc) { ssrc_ = ssrc; }
    void setPayloadType(uint8_t pt) { payload_type_ = pt; }
    void setClockRate(uint32_t rate) { clock_rate_ = rate; }
    
    // 打包一帧视频数据，返回多个RTP包
    virtual std::vector<RtpPacket> packFrame(const VideoFrame& frame) = 0;

protected:
    uint16_t getNextSeq() { return seq_++; }
    
    uint32_t ssrc_ = 0;
    uint8_t payload_type_ = 96;
    uint32_t clock_rate_ = 90000;
    uint16_t seq_ = 0;
};

// H.264 RTP打包器 (RFC 6184)
class H264RtpPacker : public RtpPacker {
public:
    H264RtpPacker();
    
    // 打包视频帧
    std::vector<RtpPacket> packFrame(const VideoFrame& frame) override;
    
    // 设置MTU
    void setMtu(size_t mtu) { mtu_ = mtu; }

private:
    // 解析NALU单元
    std::vector<NaluUnit> parseNalus(const uint8_t* data, size_t size);
    
    // 打包单个NALU
    void packSingleNalu(const NaluUnit& nalu, uint32_t timestamp, 
                        std::vector<RtpPacket>& packets);
    
    // 分片打包 (FU-A)
    void packFuA(const NaluUnit& nalu, uint32_t timestamp,
                 std::vector<RtpPacket>& packets);

private:
    size_t mtu_ = 1400;  // 默认MTU，留余量
};

// H.265 RTP打包器 (RFC 7798)
class H265RtpPacker : public RtpPacker {
public:
    H265RtpPacker();
    
    std::vector<RtpPacket> packFrame(const VideoFrame& frame) override;
    
    void setMtu(size_t mtu) { mtu_ = mtu; }

private:
    std::vector<NaluUnit> parseNalus(const uint8_t* data, size_t size);
    void packSingleNalu(const NaluUnit& nalu, uint32_t timestamp,
                        std::vector<RtpPacket>& packets);
    void packFu(const NaluUnit& nalu, uint32_t timestamp,
                std::vector<RtpPacket>& packets);

private:
    size_t mtu_ = 1400;
};

// RTP包发送器
class RtpSender {
public:
    RtpSender();
    ~RtpSender();

    bool init(const std::string& local_ip, uint16_t local_port);
    bool setPeer(const std::string& peer_ip, uint16_t peer_rtp_port, uint16_t peer_rtcp_port);
    
    // 发送RTP包
    bool sendRtpPacket(const RtpPacket& packet);
    
    // 批量发送
    bool sendRtpPackets(const std::vector<RtpPacket>& packets);
    
    // 发送RTCP包（简单的Sender Report）
    bool sendSenderReport(uint32_t rtp_timestamp, uint64_t ntp_timestamp,
                          uint32_t packet_count, uint32_t octet_count);
    
    uint16_t getLocalPort() const;
    uint16_t getLocalRtcpPort() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rtsp
