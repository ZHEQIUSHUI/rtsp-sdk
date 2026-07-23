#include "media_file.h"

// 单流封装无需 SPS/PPS ID 重排；关掉它可绕过 minimp4 的 SPS 位解析
// （其 show_bits 限 16 bit，对某些编码器(如 openh264)的 SPS 会断言/读越界）。
#define MINIMP4_TRANSCODE_SPS_ID 0
#define MINIMP4_IMPLEMENTATION
#include "../../third_party/minimp4.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace rtspcli {

using rtsp::CodecType;
using rtsp::VideoFrame;

namespace {

const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

bool endsWithCI(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower((unsigned char)s[s.size() - suffix.size() + i]) !=
            std::tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

// 一个 NAL：sc_begin=起始码首字节, payload=NAL 首字节(类型字节), end=下一个起始码首字节/缓冲尾。
struct NalRec {
    size_t sc_begin;
    size_t payload;
    size_t end;
    uint8_t type;
    bool is_vcl;
    bool is_key;
};

bool isStart4(const uint8_t* d, size_t n, size_t i) {
    return i + 4 <= n && d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1;
}
bool isStart3(const uint8_t* d, size_t n, size_t i) {
    return i + 3 <= n && d[i] == 0 && d[i+1] == 0 && d[i+2] == 1;
}

// 解析 Annex-B 缓冲为 NAL 列表。
std::vector<NalRec> parseNals(const uint8_t* d, size_t n, CodecType codec) {
    struct Raw { size_t sc; size_t payload; };
    std::vector<Raw> raw;
    for (size_t i = 0; i + 2 < n;) {
        if (isStart4(d, n, i)) { raw.push_back({i, i + 4}); i += 4; }
        else if (isStart3(d, n, i)) { raw.push_back({i, i + 3}); i += 3; }
        else ++i;
    }
    std::vector<NalRec> out;
    out.reserve(raw.size());
    for (size_t k = 0; k < raw.size(); ++k) {
        NalRec r;
        r.sc_begin = raw[k].sc;
        r.payload = raw[k].payload;
        r.end = (k + 1 < raw.size()) ? raw[k + 1].sc : n;
        r.type = 0; r.is_vcl = false; r.is_key = false;
        if (r.payload < r.end) {
            if (codec == CodecType::H264) {
                r.type = d[r.payload] & 0x1F;
                r.is_vcl = (r.type >= 1 && r.type <= 5);
                r.is_key = (r.type == 5);
            } else {
                r.type = (d[r.payload] >> 1) & 0x3F;
                r.is_vcl = (r.type <= 31);
                r.is_key = (r.type >= 16 && r.type <= 23);  // BLA/IDR/CRA (IRAP)
            }
        }
        out.push_back(r);
    }
    return out;
}

void appendAnnexB(std::vector<uint8_t>& dst, const uint8_t* nal, size_t len) {
    dst.insert(dst.end(), kStartCode, kStartCode + 4);
    dst.insert(dst.end(), nal, nal + len);
}

// ---- 最小 SPS 解析：从裸流的 SPS 拿分辨率/帧率（裸流不含其它尺寸来源）----

// 去 emulation-prevention（00 00 03 -> 00 00），得到可按 bit 解析的 RBSP。
std::vector<uint8_t> deEscape(const uint8_t* p, size_t n) {
    std::vector<uint8_t> r; r.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (i >= 2 && p[i] == 0x03 && p[i-1] == 0x00 && p[i-2] == 0x00) continue;
        r.push_back(p[i]);
    }
    return r;
}

struct BitRd {
    const uint8_t* d; size_t nbits; size_t pos = 0;
    BitRd(const uint8_t* p, size_t bytes) : d(p), nbits(bytes * 8) {}
    int bit() { if (pos >= nbits) { ++pos; return 0; } int b = (d[pos >> 3] >> (7 - (pos & 7))) & 1; ++pos; return b; }
    uint32_t u(int n) { uint32_t v = 0; while (n-- > 0) v = (v << 1) | (uint32_t)bit(); return v; }
    uint32_t ue() { int z = 0; while (pos < nbits && bit() == 0 && z < 32) ++z; uint32_t v = (z < 32) ? ((1u << z) - 1) : 0; if (z) v += u(z); return v; }
    int32_t se() { uint32_t k = ue(); return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2); }
    bool overrun() const { return pos > nbits; }
};

void skipScalingList(BitRd& b, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0) { int delta = b.se(); next = (last + delta + 256) % 256; }
        last = (next == 0) ? last : next;
    }
}

// nal 含 1 字节 NAL 头。成功填 w/h（fps 若 VUI 有 timing 则填，否则 0）。
bool parseH264Sps(const uint8_t* nal, size_t len, int& w, int& h, double& fps) {
    if (len < 2) return false;
    std::vector<uint8_t> rbsp = deEscape(nal + 1, len - 1);
    BitRd b(rbsp.data(), rbsp.size());
    uint32_t profile = b.u(8); b.u(8); b.u(8); b.ue();
    uint32_t chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        chroma = b.ue();
        if (chroma == 3) b.u(1);
        b.ue(); b.ue(); b.u(1);
        if (b.u(1)) { for (int i = 0; i < (chroma != 3 ? 8 : 12); ++i) if (b.u(1)) skipScalingList(b, i < 6 ? 16 : 64); }
    }
    b.ue();                       // log2_max_frame_num_minus4
    uint32_t poc = b.ue();
    if (poc == 0) b.ue();
    else if (poc == 1) { b.u(1); b.se(); b.se(); uint32_t k = b.ue(); for (uint32_t i = 0; i < k; ++i) b.se(); }
    b.ue();                       // max_num_ref_frames
    b.u(1);                       // gaps_in_frame_num
    uint32_t wmbs = b.ue(), hmap = b.ue();
    uint32_t frame_only = b.u(1);
    if (!frame_only) b.u(1);
    b.u(1);                       // direct_8x8
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (b.u(1)) { cl = b.ue(); cr = b.ue(); ct = b.ue(); cb = b.ue(); }
    if (b.overrun()) return false;
    w = (int)((wmbs + 1) * 16);
    h = (int)((2 - frame_only) * (hmap + 1) * 16);
    int subW = (chroma == 1 || chroma == 2) ? 2 : 1;
    int subH = (chroma == 1) ? 2 : 1;
    int cux = (chroma == 0) ? 1 : subW;
    int cuy = ((chroma == 0) ? 1 : subH) * (2 - frame_only);
    w -= (int)(cl + cr) * cux;
    h -= (int)(ct + cb) * cuy;
    fps = 0;
    if (b.u(1)) {                 // vui_parameters_present
        if (b.u(1)) { if (b.u(8) == 255) { b.u(16); b.u(16); } }   // aspect_ratio
        if (b.u(1)) b.u(1);                                        // overscan
        if (b.u(1)) { b.u(3); b.u(1); if (b.u(1)) { b.u(8); b.u(8); b.u(8); } } // video_signal
        if (b.u(1)) { b.ue(); b.ue(); }                           // chroma_loc
        if (b.u(1)) { uint32_t nuit = b.u(32); uint32_t ts = b.u(32); b.u(1);
                      if (nuit > 0) fps = (double)ts / (2.0 * nuit); }          // timing_info
    }
    return w > 0 && h > 0;
}

// HEVC：仅处理单子层(sps_max_sub_layers_minus1==0)的常见情形，取分辨率。
bool parseH265Sps(const uint8_t* nal, size_t len, int& w, int& h) {
    if (len < 3) return false;
    std::vector<uint8_t> rbsp = deEscape(nal + 2, len - 2);
    BitRd b(rbsp.data(), rbsp.size());
    b.u(4);                       // sps_video_parameter_set_id
    uint32_t maxsub = b.u(3);     // sps_max_sub_layers_minus1
    b.u(1);                       // temporal_id_nesting
    if (maxsub != 0) return false;
    for (int i = 0; i < 12; ++i) b.u(8);  // profile_tier_level general（96 bit）
    b.ue();                       // sps_seq_parameter_set_id
    uint32_t chroma = b.ue();
    if (chroma == 3) b.u(1);
    uint32_t pw = b.ue(), ph = b.ue();
    int W = (int)pw, H = (int)ph;
    if (b.u(1)) {                 // conformance_window
        uint32_t l = b.ue(), r = b.ue(), t = b.ue(), bo = b.ue();
        int subW = (chroma == 1 || chroma == 2) ? 2 : 1, subH = (chroma == 1) ? 2 : 1;
        W -= (int)(l + r) * subW; H -= (int)(t + bo) * subH;
    }
    if (b.overrun() || W <= 0 || H <= 0) return false;
    w = W; h = H;
    return true;
}

// ---------------- 裸流读 ----------------
class RawEsReader : public MediaReader {
public:
    RawEsReader(std::vector<uint8_t> buf, CodecType codec, int forced_fps)
        : buf_(std::move(buf)) {
        params_.codec = codec;
        nals_ = parseNals(buf_.data(), buf_.size(), codec);
        buildUnits();
        for (const auto& nr : nals_) {
            if (codec == CodecType::H264) {
                if (nr.type == 7 && params_.sps.empty()) params_.sps.assign(buf_.data()+nr.payload, buf_.data()+nr.end);
                else if (nr.type == 8 && params_.pps.empty()) params_.pps.assign(buf_.data()+nr.payload, buf_.data()+nr.end);
            } else {
                if (nr.type == 32 && params_.vps.empty()) params_.vps.assign(buf_.data()+nr.payload, buf_.data()+nr.end);
                else if (nr.type == 33 && params_.sps.empty()) params_.sps.assign(buf_.data()+nr.payload, buf_.data()+nr.end);
                else if (nr.type == 34 && params_.pps.empty()) params_.pps.assign(buf_.data()+nr.payload, buf_.data()+nr.end);
            }
        }
        // 解析 SPS 补分辨率/帧率（裸流唯一可得的尺寸来源）
        double sps_fps = 0; int w = 0, hh = 0;
        if (!params_.sps.empty()) {
            if (codec == CodecType::H264) {
                if (parseH264Sps(params_.sps.data(), params_.sps.size(), w, hh, sps_fps)) {
                    params_.width = w; params_.height = hh;
                }
            } else {
                if (parseH265Sps(params_.sps.data(), params_.sps.size(), w, hh)) {
                    params_.width = w; params_.height = hh;
                }
            }
        }
        fps_ = forced_fps > 0 ? forced_fps : (sps_fps > 0 ? (int)(sps_fps + 0.5) : 25);
        params_.fps = fps_;
    }

    const MediaParams& params() const override { return params_; }
    uint64_t totalUnits() const override { return units_.size(); }

    bool next(AccessUnit& au) override {
        if (idx_ >= units_.size()) return false;
        const Unit& u = units_[idx_];
        au.data.assign(buf_.data() + u.begin, buf_.data() + u.end);
        au.is_key = u.is_key;
        au.pts_ms = static_cast<uint64_t>(idx_) * 1000 / static_cast<uint64_t>(fps_);
        ++idx_;
        return true;
    }
    void rewind() override { idx_ = 0; }

private:
    struct Unit { size_t begin; size_t end; bool is_key; };
    void buildUnits() {
        bool au_has_vcl = false;
        size_t au_begin = 0;
        bool au_key = false;
        bool open = false;
        for (size_t k = 0; k < nals_.size(); ++k) {
            const NalRec& nr = nals_[k];
            if (nr.is_vcl && au_has_vcl) {
                units_.push_back({au_begin, nr.sc_begin, au_key});
                au_begin = nr.sc_begin; au_has_vcl = false; au_key = false;
            }
            if (!open) { au_begin = nr.sc_begin; open = true; }
            if (nr.is_vcl) au_has_vcl = true;
            if (nr.is_key) au_key = true;
        }
        if (open && au_begin < buf_.size()) {
            units_.push_back({au_begin, buf_.size(), au_key});
        }
    }

    std::vector<uint8_t> buf_;
    int fps_ = 25;
    MediaParams params_;
    std::vector<NalRec> nals_;
    std::vector<Unit> units_;
    size_t idx_ = 0;
};

// ---------------- 裸流写 ----------------
class RawEsWriter : public MediaWriter {
public:
    explicit RawEsWriter(std::FILE* f, bool own) : f_(f), own_(own) {}
    ~RawEsWriter() override { if (own_ && f_) std::fclose(f_); }

    bool write(const VideoFrame& frame) override {
        if (!f_ || !frame.data || frame.size == 0) return f_ != nullptr;
        return std::fwrite(frame.data, 1, frame.size, f_) == frame.size;
    }
    bool finish() override { if (f_) std::fflush(f_); return true; }

private:
    std::FILE* f_;
    bool own_;
};

// ---------------- mp4 写（minimp4） ----------------
class Mp4Writer : public MediaWriter {
public:
    static Mp4Writer* create(const std::string& path, CodecType codec, int width, int height, int fps,
                             const std::vector<uint8_t>& vps, const std::vector<uint8_t>& sps,
                             const std::vector<uint8_t>& pps, std::string& err) {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { err = "无法创建输出文件: " + path; return nullptr; }
        MP4E_mux_t* mux = MP4E_open(/*sequential*/0, /*fragmentation*/0, f, &Mp4Writer::onWrite);
        if (!mux) { std::fclose(f); err = "MP4E_open 失败"; return nullptr; }
        auto* w = new Mp4Writer();
        w->f_ = f; w->mux_ = mux; w->fps_ = fps > 0 ? fps : 25;
        w->codec_ = codec; w->vps_ = vps; w->sps_ = sps; w->pps_ = pps;
        if (mp4_h26x_write_init(&w->h_, mux, width > 0 ? width : 1920, height > 0 ? height : 1080,
                                codec == CodecType::H265 ? 1 : 0) != MP4E_STATUS_OK) {
            // 不在此手动 MP4E_close/fclose：w->mux_/w->f_ 已赋值，delete w 的析构会
            // 一次性清理（inited_=false 故跳过 write_close）。手动再关会造成 double-free。
            delete w; err = "mp4_h26x_write_init 失败"; return nullptr;
        }
        w->inited_ = true;
        return w;
    }
    ~Mp4Writer() override {
        if (inited_) mp4_h26x_write_close(&h_);
        if (mux_) MP4E_close(mux_);
        if (f_) std::fclose(f_);
    }

    bool write(const VideoFrame& frame) override {
        if (!frame.data || frame.size == 0) return true;
        const unsigned dur = static_cast<unsigned>(90000 / (fps_ > 0 ? fps_ : 25));
        // 首帧补齐参数集，保证 minimp4 拿到 SPS/PPS(/VPS)
        if (!first_done_) {
            first_done_ = true;
            std::vector<uint8_t> head;
            if (codec_ == CodecType::H265 && !vps_.empty()) appendAnnexB(head, vps_.data(), vps_.size());
            if (!sps_.empty()) appendAnnexB(head, sps_.data(), sps_.size());
            if (!pps_.empty()) appendAnnexB(head, pps_.data(), pps_.size());
            if (!head.empty()) {
                if (mp4_h26x_write_nal(&h_, head.data(), (int)head.size(), dur) != MP4E_STATUS_OK)
                    return false;
            }
        }
        return mp4_h26x_write_nal(&h_, frame.data, (int)frame.size, dur) == MP4E_STATUS_OK;
    }
    bool finish() override { return true; }  // 真正收尾在析构（顺序：write_close→MP4E_close→fclose）

private:
    static int onWrite(int64_t offset, const void* buffer, size_t size, void* token) {
        std::FILE* f = static_cast<std::FILE*>(token);
        if (std::fseek(f, (long)offset, SEEK_SET) != 0) return -1;
        return std::fwrite(buffer, 1, size, f) == size ? 0 : -1;
    }

    std::FILE* f_ = nullptr;
    MP4E_mux_t* mux_ = nullptr;
    mp4_h26x_writer_t h_{};
    bool inited_ = false;
    bool first_done_ = false;
    int fps_ = 25;
    CodecType codec_ = CodecType::H264;
    std::vector<uint8_t> vps_, sps_, pps_;
};

// ---------------- mp4 读（minimp4） ----------------
class Mp4Reader : public MediaReader {
public:
    static Mp4Reader* open(const std::string& path, int forced_fps, std::string& err) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { err = "无法打开输入文件: " + path; return nullptr; }
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0) { std::fclose(f); err = "输入文件为空"; return nullptr; }

        auto* r = new Mp4Reader();
        r->f_ = f; r->file_size_ = size;
        if (MP4D_open(&r->demux_, &Mp4Reader::onRead, f, size) != 1) {
            delete r; err = "MP4D_open 失败（不是有效 mp4？）"; return nullptr;
        }
        // 找视频轨
        int vtrack = -1;
        for (unsigned t = 0; t < r->demux_.track_count; ++t) {
            unsigned ot = r->demux_.track[t].object_type_indication;
            if (ot == MP4_OBJECT_TYPE_AVC || ot == MP4_OBJECT_TYPE_HEVC) { vtrack = (int)t; break; }
        }
        if (vtrack < 0) { delete r; err = "mp4 中没有 H.264/H.265 视频轨"; return nullptr; }
        r->track_ = (unsigned)vtrack;
        const MP4D_track_t& tr = r->demux_.track[vtrack];
        r->params_.codec = (tr.object_type_indication == MP4_OBJECT_TYPE_HEVC) ? CodecType::H265 : CodecType::H264;
        r->sample_count_ = tr.sample_count;
#if MP4D_INFO_SUPPORTED
        r->params_.width = (int)tr.SampleDescription.video.width;
        r->params_.height = (int)tr.SampleDescription.video.height;
        r->timescale_ = tr.timescale ? tr.timescale : 90000;
#else
        r->timescale_ = 90000;
#endif
        r->extractParamSets(tr);
        // NAL length 前缀宽度：minimp4 demux 解析 avcC 时丢弃了 lengthSizeMinusOne，
        // 故从首样本自动探测（取能恰好消费完整样本的宽度，默认 4）。
        r->length_size_ = 4;
        if (r->sample_count_ > 0) {
            unsigned b = 0, ts = 0, dur = 0;
            MP4D_file_offset_t off0 = MP4D_frame_offset(&r->demux_, r->track_, 0, &b, &ts, &dur);
            if (b > 0) {
                std::vector<uint8_t> s0(b);
                if (std::fseek(f, (long)off0, SEEK_SET) == 0 && std::fread(s0.data(), 1, b, f) == b) {
                    r->length_size_ = detectLengthSize(s0.data(), b);
                }
            }
        }

        // fps：forced 优先；否则用首样本时长估算
        if (forced_fps > 0) {
            r->params_.fps = forced_fps;
        } else if (r->sample_count_ > 0) {
            unsigned bytes = 0, ts = 0, dur = 0;
            MP4D_frame_offset(&r->demux_, r->track_, 0, &bytes, &ts, &dur);
            r->params_.fps = (dur > 0) ? (int)((r->timescale_ + dur / 2) / dur) : 25;
            if (r->params_.fps <= 0) r->params_.fps = 25;
        } else {
            r->params_.fps = 25;
        }
        return r;
    }
    ~Mp4Reader() override {
        MP4D_close(&demux_);
        if (f_) std::fclose(f_);
    }

    const MediaParams& params() const override { return params_; }
    uint64_t totalUnits() const override { return sample_count_; }

    bool next(AccessUnit& au) override {
        if (sample_idx_ >= sample_count_) return false;
        unsigned bytes = 0, ts = 0, dur = 0;
        MP4D_file_offset_t off = MP4D_frame_offset(&demux_, track_, sample_idx_, &bytes, &ts, &dur);
        std::vector<uint8_t> sample(bytes);
        if (bytes > 0) {
            if (std::fseek(f_, (long)off, SEEK_SET) != 0) return false;
            if (std::fread(sample.data(), 1, bytes, f_) != bytes) return false;
        }
        au.data.clear();
        au.is_key = false;
        // length-prefixed → Annex-B
        bool has_vcl_key = false;
        size_t p = 0;
        while (p + length_size_ <= sample.size()) {
            uint32_t nlen = 0;
            for (int b = 0; b < length_size_; ++b) nlen = (nlen << 8) | sample[p + b];
            p += length_size_;
            if (nlen == 0 || p + nlen > sample.size()) break;
            const uint8_t* nal = sample.data() + p;
            if (params_.codec == CodecType::H264) {
                uint8_t t = nal[0] & 0x1F;
                if (t == 5) has_vcl_key = true;
            } else {
                uint8_t t = (nal[0] >> 1) & 0x3F;
                if (t >= 16 && t <= 23) has_vcl_key = true;
            }
            appendAnnexB(au.data, nal, nlen);
            p += nlen;
        }
        au.is_key = has_vcl_key;
        // 关键帧前补齐参数集，便于服务端 auto-extract
        if (has_vcl_key) {
            std::vector<uint8_t> head;
            if (params_.codec == CodecType::H265 && !params_.vps.empty()) appendAnnexB(head, params_.vps.data(), params_.vps.size());
            if (!params_.sps.empty()) appendAnnexB(head, params_.sps.data(), params_.sps.size());
            if (!params_.pps.empty()) appendAnnexB(head, params_.pps.data(), params_.pps.size());
            if (!head.empty()) au.data.insert(au.data.begin(), head.begin(), head.end());
        }
        au.pts_ms = (timescale_ > 0) ? (uint64_t)ts * 1000 / timescale_
                                     : (uint64_t)sample_idx_ * 1000 / (params_.fps > 0 ? params_.fps : 25);
        ++sample_idx_;
        return true;
    }
    void rewind() override { sample_idx_ = 0; }

private:
    static int onRead(int64_t offset, void* buffer, size_t size, void* token) {
        std::FILE* f = static_cast<std::FILE*>(token);
        if (std::fseek(f, (long)offset, SEEK_SET) != 0) return -1;
        return std::fread(buffer, 1, size, f) == size ? 0 : -1;
    }

    // 自动探测 NAL length 前缀宽度：选能恰好消费完整样本的宽度（优先 4）。
    static int detectLengthSize(const uint8_t* s, size_t n) {
        const int cands[4] = {4, 2, 1, 3};
        for (int ci = 0; ci < 4; ++ci) {
            int ls = cands[ci];
            size_t p = 0; int nals = 0; bool ok = true;
            while (p + (size_t)ls <= n) {
                uint32_t len = 0;
                for (int b = 0; b < ls; ++b) len = (len << 8) | s[p + b];
                p += ls;
                if (len == 0 || p + len > n) { ok = false; break; }
                p += len; ++nals;
            }
            if (ok && p == n && nals > 0) return ls;
        }
        return 4;
    }

    void extractParamSets(const MP4D_track_t& tr) {
        if (params_.codec == CodecType::H264) {
            int n = 0;
            if (const void* s = MP4D_read_sps(&demux_, track_, 0, &n)) params_.sps.assign((const uint8_t*)s, (const uint8_t*)s + n);
            n = 0;
            if (const void* p = MP4D_read_pps(&demux_, track_, 0, &n)) params_.pps.assign((const uint8_t*)p, (const uint8_t*)p + n);
        } else {
            // 手动解析 hvcC：22 字节固定头后 numOfArrays，每个 array: type(1)+numNalus(2)+(len(2)+data)*
            const uint8_t* d = tr.dsi; size_t n = tr.dsi_bytes;
            if (!d || n < 23) return;
            size_t p = 22;
            uint8_t num_arrays = d[p++];
            for (uint8_t a = 0; a < num_arrays && p + 3 <= n; ++a) {
                uint8_t nal_type = d[p] & 0x3F; p += 1;
                uint16_t cnt = (d[p] << 8) | d[p + 1]; p += 2;
                for (uint16_t c = 0; c < cnt && p + 2 <= n; ++c) {
                    uint16_t len = (d[p] << 8) | d[p + 1]; p += 2;
                    if (p + len > n) return;
                    if (nal_type == 32 && params_.vps.empty()) params_.vps.assign(d + p, d + p + len);
                    else if (nal_type == 33 && params_.sps.empty()) params_.sps.assign(d + p, d + p + len);
                    else if (nal_type == 34 && params_.pps.empty()) params_.pps.assign(d + p, d + p + len);
                    p += len;
                }
            }
        }
    }

    std::FILE* f_ = nullptr;
    long file_size_ = 0;
    MP4D_demux_t demux_{};
    unsigned track_ = 0;
    unsigned sample_count_ = 0;
    unsigned sample_idx_ = 0;
    unsigned timescale_ = 90000;
    int length_size_ = 4;
    MediaParams params_;
};

} // namespace

bool isMp4Path(const std::string& path) { return endsWithCI(path, ".mp4"); }

std::unique_ptr<MediaReader> MediaReader::open(const std::string& path, int forced_fps, std::string& err) {
    if (isMp4Path(path)) {
        return std::unique_ptr<MediaReader>(Mp4Reader::open(path, forced_fps, err));
    }
    // 裸流：按扩展名定 codec（.h265/.hevc=H265，否则 H264）
    CodecType codec = (endsWithCI(path, ".h265") || endsWithCI(path, ".hevc") || endsWithCI(path, ".265"))
                          ? CodecType::H265 : CodecType::H264;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "无法打开输入文件: " + path; return nullptr; }
    std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); err = "输入文件为空"; return nullptr; }
    std::vector<uint8_t> buf((size_t)sz);
    size_t rd = std::fread(buf.data(), 1, (size_t)sz, f);
    std::fclose(f);
    buf.resize(rd);
    return std::unique_ptr<MediaReader>(new RawEsReader(std::move(buf), codec, forced_fps));
}

std::unique_ptr<MediaWriter> MediaWriter::create(const std::string& path, CodecType codec,
                                                 int width, int height, int fps,
                                                 const std::vector<uint8_t>& vps,
                                                 const std::vector<uint8_t>& sps,
                                                 const std::vector<uint8_t>& pps,
                                                 std::string& err) {
    if (isMp4Path(path)) {
        return std::unique_ptr<MediaWriter>(Mp4Writer::create(path, codec, width, height, fps, vps, sps, pps, err));
    }
    std::FILE* f = nullptr; bool own = true;
    if (path == "-") { f = stdout; own = false; }
    else { f = std::fopen(path.c_str(), "wb"); if (!f) { err = "无法创建输出文件: " + path; return nullptr; } }
    return std::unique_ptr<MediaWriter>(new RawEsWriter(f, own));
}

} // namespace rtspcli
