#include <rtsp-common/rtp_packer.h>
#include <rtsp-common/common.h>
#include <rtsp-common/socket.h>
#include <cstring>
#include <arpa/inet.h>

namespace rtsp {

// H264 NALU类型
enum H264NaluType {
    H264_NALU_SLICE = 1,
    H264_NALU_DPA = 2,
    H264_NALU_DPB = 3,
    H264_NALU_DPC = 4,
    H264_NALU_IDR = 5,
    H264_NALU_SEI = 6,
    H264_NALU_SPS = 7,
    H264_NALU_PPS = 8,
    H264_NALU_AUD = 9,
    H264_NALU_EOSEQ = 10,
    H264_NALU_EOSTREAM = 11,
    H264_NALU_FILL = 12,
    H264_NALU_PREFIX = 14,
    H264_NALU_SUB_SPS = 15,
    H264_NALU_SLC_EXT = 20,
    H264_NALA_VDRD = 24  // View and Dependency Representation Delimiter NAL Unit
};

// H265 NALU类型
enum H265NaluType {
    H265_NALU_TRAIL_N = 0,
    H265_NALU_TRAIL_R = 1,
    H265_NALU_TSA_N = 2,
    H265_NALU_TSA_R = 3,
    H265_NALU_STSA_N = 4,
    H265_NALU_STSA_R = 5,
    H265_NALU_RADL_N = 6,
    H265_NALU_RADL_R = 7,
    H265_NALU_RASL_N = 8,
    H265_NALU_RASL_R = 9,
    H265_NALU_BLA_W_LP = 16,
    H265_NALU_BLA_W_RADL = 17,
    H265_NALU_BLA_N_LP = 18,
    H265_NALU_IDR_W_RADL = 19,
    H265_NALU_IDR_N_LP = 20,
    H265_NALU_CRA_NUT = 21,
    H265_NALU_RSV_IRAP_VCL22 = 22,
    H265_NALU_RSV_IRAP_VCL23 = 23,
    H265_NALU_VPS = 32,
    H265_NALU_SPS = 33,
    H265_NALU_PPS = 34,
    H265_NALU_AUD = 35,
    H265_NALU_EOS_NUT = 36,
    H265_NALU_EOB_NUT = 37,
    H265_NALU_FD_NUT = 38,
    H265_NALU_PREFIX_SEI_NUT = 39,
    H265_NALU_SUFFIX_SEI_NUT = 40
};

// 查找NALU起始码
static const uint8_t* findStartCode(const uint8_t* data, size_t size, size_t& start_code_len) {
    for (size_t i = 0; i + 3 < size; i++) {
        if (data[i] == 0 && data[i+1] == 0) {
            if (data[i+2] == 1) {
                start_code_len = 3;
                return data + i;
            } else if (i + 4 < size && data[i+2] == 0 && data[i+3] == 1) {
                start_code_len = 4;
                return data + i;
            }
        }
    }
    return nullptr;
}

// 解析NALU列表
static std::vector<NaluUnit> parseNalusInternal(const uint8_t* data, size_t size) {
    std::vector<NaluUnit> nalus;
    
    size_t offset = 0;
    while (offset + 3 < size) {
        // 查找起始码
        size_t start_code_len = 0;
        const uint8_t* start = findStartCode(data + offset, size - offset, start_code_len);
        
        if (!start) {
            // 没有找到更多起始码，处理剩余数据作为最后一个NALU
            if (!nalus.empty() && nalus.back().size == 0) {
                // 设置前一个NALU的大小到数据末尾
                nalus.back().size = size - offset;
            } else if (offset < size && nalus.empty()) {
                // 没有起始码的裸NALU数据
                NaluUnit nalu;
                nalu.data = data + offset;
                nalu.size = size - offset;
                nalu.start_code = false;
                nalu.type = nalu.size > 0 ? (nalu.data[0] & 0x1F) : 0;
                nalus.push_back(nalu);
            }
            break;
        }
        
        // 计算NALU数据开始位置（跳过起始码）
        size_t nalu_start = (start - data) + start_code_len;
        
        // 如果有前一个NALU且大小未设置，设置其大小
        if (!nalus.empty() && nalus.back().size == 0) {
            // 使用指针差值，确保不为负
            if (start >= data + offset) {
                nalus.back().size = static_cast<size_t>(start - (data + offset));
            }
        }
        
        // 添加新NALU（大小稍后设置）
        NaluUnit nalu;
        nalu.data = data + nalu_start;
        nalu.size = 0;  // 稍后设置
        nalu.start_code = true;
        nalu.type = (nalu_start < size) ? (data[nalu_start] & 0x1F) : 0;
        nalus.push_back(nalu);
        
        // 更新offset到NALU数据开始处，继续查找下一个起始码
        offset = nalu_start;
    }
    
    // 处理最后一个NALU的大小
    if (!nalus.empty() && nalus.back().size == 0) {
        nalus.back().size = size - offset;
    }
    
    return nalus;
}

// H264RtpPacker实现
H264RtpPacker::H264RtpPacker() {
    ssrc_ = 0x12345678;  // 默认SSRC
    seq_ = 0;
    payload_type_ = 96;
    clock_rate_ = 90000;
}

std::vector<NaluUnit> H264RtpPacker::parseNalus(const uint8_t* data, size_t size) {
    return parseNalusInternal(data, size);
}

std::vector<RtpPacket> H264RtpPacker::packFrame(const VideoFrame& frame) {
    std::vector<RtpPacket> packets;
    
    uint32_t rtp_timestamp = convertToRtpTimestamp(frame.pts, clock_rate_);
    
    // 解析NALU
    auto nalus = parseNalus(frame.data, frame.size);
    
    for (const auto& nalu : nalus) {
        if (nalu.size == 0) continue;
        
        // 去除起始码后的实际数据
        const uint8_t* nalu_data = nalu.data;
        size_t nalu_size = nalu.size;
        
        // 跳过可能的起始码
        if (nalu_size > 3 && nalu_data[0] == 0 && nalu_data[1] == 0 && nalu_data[2] == 1) {
            nalu_data += 3;
            nalu_size -= 3;
        } else if (nalu_size > 4 && nalu_data[0] == 0 && nalu_data[1] == 0 && 
                   nalu_data[2] == 0 && nalu_data[3] == 1) {
            nalu_data += 4;
            nalu_size -= 4;
        }
        
        if (nalu_size == 0) continue;
        
        // 小于MTU的NALU直接打包
        if (nalu_size <= mtu_) {
            packSingleNalu(nalu, rtp_timestamp, packets);
        } else {
            // 大NALU分片
            packFuA(nalu, rtp_timestamp, packets);
        }
    }
    
    // 设置最后一个包的marker位
    if (!packets.empty()) {
        packets.back().marker = true;
    }
    
    return packets;
}

void H264RtpPacker::packSingleNalu(const NaluUnit& nalu, uint32_t timestamp,
                                    std::vector<RtpPacket>& packets) {
    const uint8_t* nalu_data = nalu.data;
    size_t nalu_size = nalu.size;
    
    // 跳过起始码
    if (nalu_size > 3 && nalu_data[0] == 0 && nalu_data[1] == 0 && nalu_data[2] == 1) {
        nalu_data += 3;
        nalu_size -= 3;
    } else if (nalu_size > 4 && nalu_data[0] == 0 && nalu_data[1] == 0 && 
               nalu_data[2] == 0 && nalu_data[3] == 1) {
        nalu_data += 4;
        nalu_size -= 4;
    }
    
    if (nalu_size == 0) return;
    
    // 构建RTP包
    size_t packet_size = 12 + nalu_size;  // RTP头 + NALU
    RtpPacket packet;
    packet.data = new uint8_t[packet_size];
    packet.size = packet_size;
    packet.seq = getNextSeq();
    packet.timestamp = timestamp;
    packet.ssrc = ssrc_;
    packet.marker = false;
    
    // RTP头 (12字节)
    uint8_t* p = packet.data;
    p[0] = 0x80;  // V=2, P=0, X=0, CC=0
    p[1] = payload_type_;
    p[2] = (packet.seq >> 8) & 0xFF;
    p[3] = packet.seq & 0xFF;
    p[4] = (timestamp >> 24) & 0xFF;
    p[5] = (timestamp >> 16) & 0xFF;
    p[6] = (timestamp >> 8) & 0xFF;
    p[7] = timestamp & 0xFF;
    p[8] = (ssrc_ >> 24) & 0xFF;
    p[9] = (ssrc_ >> 16) & 0xFF;
    p[10] = (ssrc_ >> 8) & 0xFF;
    p[11] = ssrc_ & 0xFF;
    
    // 复制NALU数据
    memcpy(p + 12, nalu_data, nalu_size);
    
    packets.push_back(packet);
}

void H264RtpPacker::packFuA(const NaluUnit& nalu, uint32_t timestamp,
                             std::vector<RtpPacket>& packets) {
    const uint8_t* nalu_data = nalu.data;
    size_t nalu_size = nalu.size;
    
    // 跳过起始码
    if (nalu_size > 3 && nalu_data[0] == 0 && nalu_data[1] == 0 && nalu_data[2] == 1) {
        nalu_data += 3;
        nalu_size -= 3;
    } else if (nalu_size > 4 && nalu_data[0] == 0 && nalu_data[1] == 0 && 
               nalu_data[2] == 0 && nalu_data[3] == 1) {
        nalu_data += 4;
        nalu_size -= 4;
    }
    
    if (nalu_size <= 1) return;
    
    uint8_t nalu_header = nalu_data[0];
    uint8_t nalu_type = nalu_header & 0x1F;
    uint8_t nri = (nalu_header >> 5) & 0x03;
    
    const uint8_t* payload = nalu_data + 1;
    size_t payload_size = nalu_size - 1;
    
    size_t max_fragment_size = mtu_ - 2;  // 减去FU indicator和FU header
    
    size_t offset = 0;
    bool first = true;
    
    while (offset < payload_size) {
        size_t fragment_size = std::min(max_fragment_size, payload_size - offset);
        bool last = (offset + fragment_size >= payload_size);
        
        size_t packet_size = 12 + 2 + fragment_size;  // RTP头 + FU头 + 数据
        RtpPacket packet;
        packet.data = new uint8_t[packet_size];
        packet.size = packet_size;
        packet.seq = getNextSeq();
        packet.timestamp = timestamp;
        packet.ssrc = ssrc_;
        packet.marker = last;
        
        uint8_t* p = packet.data;
        // RTP头
        p[0] = 0x80;
        p[1] = payload_type_;
        p[2] = (packet.seq >> 8) & 0xFF;
        p[3] = packet.seq & 0xFF;
        p[4] = (timestamp >> 24) & 0xFF;
        p[5] = (timestamp >> 16) & 0xFF;
        p[6] = (timestamp >> 8) & 0xFF;
        p[7] = timestamp & 0xFF;
        p[8] = (ssrc_ >> 24) & 0xFF;
        p[9] = (ssrc_ >> 16) & 0xFF;
        p[10] = (ssrc_ >> 8) & 0xFF;
        p[11] = ssrc_ & 0xFF;
        
        // FU indicator
        p[12] = (nri << 5) | 28;  // 28 = FU-A
        
        // FU header
        uint8_t fu_header = nalu_type;
        if (first) fu_header |= 0x80;  // S位
        if (last) fu_header |= 0x40;   // E位
        p[13] = fu_header;
        
        // 复制数据
        memcpy(p + 14, payload + offset, fragment_size);
        
        packets.push_back(packet);
        
        offset += fragment_size;
        first = false;
    }
}

// H265RtpPacker实现
H265RtpPacker::H265RtpPacker() {
    ssrc_ = 0x12345678;
    seq_ = 0;
    payload_type_ = 96;
    clock_rate_ = 90000;
}

std::vector<NaluUnit> H265RtpPacker::parseNalus(const uint8_t* data, size_t size) {
    return parseNalusInternal(data, size);
}

std::vector<RtpPacket> H265RtpPacker::packFrame(const VideoFrame& frame) {
    std::vector<RtpPacket> packets;
    
    uint32_t rtp_timestamp = convertToRtpTimestamp(frame.pts, clock_rate_);
    
    auto nalus = parseNalus(frame.data, frame.size);
    
    for (const auto& nalu : nalus) {
        if (nalu.size == 0) continue;
        
        const uint8_t* nalu_data = nalu.data;
        size_t nalu_size = nalu.size;
        
        // 跳过起始码
        if (nalu_size > 3 && nalu_data[0] == 0 && nalu_data[1] == 0 && nalu_data[2] == 1) {
            nalu_data += 3;
            nalu_size -= 3;
        } else if (nalu_size > 4 && nalu_data[0] == 0 && nalu_data[1] == 0 && 
                   nalu_data[2] == 0 && nalu_data[3] == 1) {
            nalu_data += 4;
            nalu_size -= 4;
        }
        
        if (nalu_size < 2) continue;
        
        if (nalu_size <= mtu_) {
            packSingleNalu(nalu, rtp_timestamp, packets);
        } else {
            packFu(nalu, rtp_timestamp, packets);
        }
    }
    
    if (!packets.empty()) {
        packets.back().marker = true;
    }
    
    return packets;
}

void H265RtpPacker::packSingleNalu(const NaluUnit& nalu, uint32_t timestamp,
                                    std::vector<RtpPacket>& packets) {
    const uint8_t* nalu_data = nalu.data;
    size_t nalu_size = nalu.size;
    
    // 跳过起始码
    if (nalu_size > 3 && nalu_data[0] == 0 && nalu_data[1] == 0 && nalu_data[2] == 1) {
        nalu_data += 3;
        nalu_size -= 3;
    } else if (nalu_size > 4 && nalu_data[0] == 0 && nalu_data[1] == 0 && 
               nalu_data[2] == 0 && nalu_data[3] == 1) {
        nalu_data += 4;
        nalu_size -= 4;
    }
    
    if (nalu_size < 2) return;
    
    size_t packet_size = 12 + nalu_size;
    RtpPacket packet;
    packet.data = new uint8_t[packet_size];
    packet.size = packet_size;
    packet.seq = getNextSeq();
    packet.timestamp = timestamp;
    packet.ssrc = ssrc_;
    packet.marker = false;
    
    uint8_t* p = packet.data;
    p[0] = 0x80;
    p[1] = payload_type_;
    p[2] = (packet.seq >> 8) & 0xFF;
    p[3] = packet.seq & 0xFF;
    p[4] = (timestamp >> 24) & 0xFF;
    p[5] = (timestamp >> 16) & 0xFF;
    p[6] = (timestamp >> 8) & 0xFF;
    p[7] = timestamp & 0xFF;
    p[8] = (ssrc_ >> 24) & 0xFF;
    p[9] = (ssrc_ >> 16) & 0xFF;
    p[10] = (ssrc_ >> 8) & 0xFF;
    p[11] = ssrc_ & 0xFF;
    
    memcpy(p + 12, nalu_data, nalu_size);
    
    packets.push_back(packet);
}

void H265RtpPacker::packFu(const NaluUnit& nalu, uint32_t timestamp,
                            std::vector<RtpPacket>& packets) {
    const uint8_t* nalu_data = nalu.data;
    size_t nalu_size = nalu.size;
    
    // 跳过起始码
    if (nalu_size > 3 && nalu_data[0] == 0 && nalu_data[1] == 0 && nalu_data[2] == 1) {
        nalu_data += 3;
        nalu_size -= 3;
    } else if (nalu_size > 4 && nalu_data[0] == 0 && nalu_data[1] == 0 && 
               nalu_data[2] == 0 && nalu_data[3] == 1) {
        nalu_data += 4;
        nalu_size -= 4;
    }
    
    if (nalu_size <= 2) return;
    
    uint8_t nalu_header[2];
    nalu_header[0] = nalu_data[0];
    nalu_header[1] = nalu_data[1];
    
    uint8_t nalu_type = (nalu_header[0] >> 1) & 0x3F;
    uint8_t nuh_layer_id = ((nalu_header[0] & 0x01) << 5) | ((nalu_header[1] >> 3) & 0x1F);
    uint8_t nuh_temporal_id_plus1 = nalu_header[1] & 0x07;
    
    const uint8_t* payload = nalu_data + 2;
    size_t payload_size = nalu_size - 2;
    
    size_t max_fragment_size = mtu_ - 3;  // 减去FU header的3字节
    
    size_t offset = 0;
    bool first = true;
    
    while (offset < payload_size) {
        size_t fragment_size = std::min(max_fragment_size, payload_size - offset);
        bool last = (offset + fragment_size >= payload_size);
        
        size_t packet_size = 12 + 3 + fragment_size;
        RtpPacket packet;
        packet.data = new uint8_t[packet_size];
        packet.size = packet_size;
        packet.seq = getNextSeq();
        packet.timestamp = timestamp;
        packet.ssrc = ssrc_;
        packet.marker = last;
        
        uint8_t* p = packet.data;
        // RTP头
        p[0] = 0x80;
        p[1] = payload_type_;
        p[2] = (packet.seq >> 8) & 0xFF;
        p[3] = packet.seq & 0xFF;
        p[4] = (timestamp >> 24) & 0xFF;
        p[5] = (timestamp >> 16) & 0xFF;
        p[6] = (timestamp >> 8) & 0xFF;
        p[7] = timestamp & 0xFF;
        p[8] = (ssrc_ >> 24) & 0xFF;
        p[9] = (ssrc_ >> 16) & 0xFF;
        p[10] = (ssrc_ >> 8) & 0xFF;
        p[11] = ssrc_ & 0xFF;
        
        // FU header (3字节)
        // 第一个字节: 0 | 1 | nuh_temporal_id_plus1 (6 bits, but only 3 used) -> actually FU header uses 49
        p[12] = 49 << 1;  // FU type = 49 (PayloadHdr)
        p[12] |= (nuh_layer_id >> 5) & 0x01;
        p[13] = ((nuh_layer_id & 0x1F) << 3) | nuh_temporal_id_plus1;
        
        // FU header byte
        p[14] = nalu_type;
        if (first) p[14] |= 0x80;  // S
        if (last) p[14] |= 0x40;   // E
        
        memcpy(p + 15, payload + offset, fragment_size);
        
        packets.push_back(packet);
        
        offset += fragment_size;
        first = false;
    }
}

// RtpSender实现
class RtpSender::Impl {
public:
    Socket rtp_socket_;
    Socket rtcp_socket_;
    std::string peer_ip_;
    uint16_t peer_rtp_port_ = 0;
    uint16_t peer_rtcp_port_ = 0;
};

RtpSender::RtpSender() : impl_(std::make_unique<Impl>()) {}
RtpSender::~RtpSender() = default;

bool RtpSender::init(const std::string& local_ip, uint16_t local_port) {
    // RTP socket
    if (!impl_->rtp_socket_.bindUdp(local_ip, local_port)) {
        return false;
    }
    
    // RTCP socket (RTP端口 + 1)
    if (!impl_->rtcp_socket_.bindUdp(local_ip, local_port + 1)) {
        impl_->rtp_socket_.close();
        return false;
    }
    
    return true;
}

bool RtpSender::setPeer(const std::string& peer_ip, uint16_t peer_rtp_port, uint16_t peer_rtcp_port) {
    impl_->peer_ip_ = peer_ip;
    impl_->peer_rtp_port_ = peer_rtp_port;
    impl_->peer_rtcp_port_ = peer_rtcp_port;
    return true;
}

bool RtpSender::sendRtpPacket(const RtpPacket& packet) {
    if (impl_->peer_rtp_port_ == 0) return false;
    
    ssize_t sent = impl_->rtp_socket_.sendTo(packet.data, packet.size, 
                                              impl_->peer_ip_, impl_->peer_rtp_port_);
    return sent == static_cast<ssize_t>(packet.size);
}

bool RtpSender::sendRtpPackets(const std::vector<RtpPacket>& packets) {
    for (const auto& packet : packets) {
        if (!sendRtpPacket(packet)) {
            return false;
        }
    }
    return true;
}

bool RtpSender::sendSenderReport(uint32_t rtp_timestamp, uint64_t ntp_timestamp,
                                  uint32_t packet_count, uint32_t octet_count) {
    if (impl_->peer_rtcp_port_ == 0) return false;
    
    // 构建RTCP Sender Report
    uint8_t sr[52];
    memset(sr, 0, sizeof(sr));
    
    // RTCP header
    sr[0] = 0x80;  // V=2, P=0, RC=0
    sr[1] = 200;   // PT=SR
    sr[2] = 0;     // length (in 32-bit words minus one)
    sr[3] = 12;    // = 52/4 - 1 = 12
    
    // SSRC (使用固定值，实际应该存储会话的SSRC)
    uint32_t ssrc = 0x12345678;
    sr[4] = (ssrc >> 24) & 0xFF;
    sr[5] = (ssrc >> 16) & 0xFF;
    sr[6] = (ssrc >> 8) & 0xFF;
    sr[7] = ssrc & 0xFF;
    
    // NTP timestamp (64-bit)
    sr[8] = (ntp_timestamp >> 56) & 0xFF;
    sr[9] = (ntp_timestamp >> 48) & 0xFF;
    sr[10] = (ntp_timestamp >> 40) & 0xFF;
    sr[11] = (ntp_timestamp >> 32) & 0xFF;
    sr[12] = (ntp_timestamp >> 24) & 0xFF;
    sr[13] = (ntp_timestamp >> 16) & 0xFF;
    sr[14] = (ntp_timestamp >> 8) & 0xFF;
    sr[15] = ntp_timestamp & 0xFF;
    
    // RTP timestamp
    sr[16] = (rtp_timestamp >> 24) & 0xFF;
    sr[17] = (rtp_timestamp >> 16) & 0xFF;
    sr[18] = (rtp_timestamp >> 8) & 0xFF;
    sr[19] = rtp_timestamp & 0xFF;
    
    // packet count
    sr[20] = (packet_count >> 24) & 0xFF;
    sr[21] = (packet_count >> 16) & 0xFF;
    sr[22] = (packet_count >> 8) & 0xFF;
    sr[23] = packet_count & 0xFF;
    
    // octet count
    sr[24] = (octet_count >> 24) & 0xFF;
    sr[25] = (octet_count >> 16) & 0xFF;
    sr[26] = (octet_count >> 8) & 0xFF;
    sr[27] = octet_count & 0xFF;
    
    ssize_t sent = impl_->rtcp_socket_.sendTo(sr, 28, impl_->peer_ip_, impl_->peer_rtcp_port_);
    return sent == 28;
}

uint16_t RtpSender::getLocalPort() const {
    return impl_->rtp_socket_.getLocalPort();
}

uint16_t RtpSender::getLocalRtcpPort() const {
    return impl_->rtcp_socket_.getLocalPort();
}

} // namespace rtsp
