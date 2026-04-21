#include "wsse_auth.h"

#include <rtsp-common/common.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <regex>
#include <sstream>
#include <vector>

namespace rtsp {

namespace {

// ---- Minimal SHA1 (public domain style, based on RFC 3174) ------------------

struct Sha1Ctx {
    uint32_t h[5];
    uint64_t bit_len;
    uint8_t  buf[64];
    size_t   buf_len;
};

inline uint32_t rol(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32 - n));
}

void sha1Init(Sha1Ctx& c) {
    c.h[0] = 0x67452301;
    c.h[1] = 0xEFCDAB89;
    c.h[2] = 0x98BADCFE;
    c.h[3] = 0x10325476;
    c.h[4] = 0xC3D2E1F0;
    c.bit_len = 0;
    c.buf_len = 0;
}

void sha1Transform(Sha1Ctx& c, const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16)
             | (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;           k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;           k = 0xCA62C1D6; }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d; c.h[4] += e;
}

void sha1Update(Sha1Ctx& c, const uint8_t* data, size_t len) {
    c.bit_len += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        const size_t take = std::min(size_t{64} - c.buf_len, len);
        std::memcpy(c.buf + c.buf_len, data, take);
        c.buf_len += take;
        data += take;
        len  -= take;
        if (c.buf_len == 64) {
            sha1Transform(c, c.buf);
            c.buf_len = 0;
        }
    }
}

void sha1Final(Sha1Ctx& c, uint8_t out[20]) {
    c.buf[c.buf_len++] = 0x80;
    if (c.buf_len > 56) {
        while (c.buf_len < 64) c.buf[c.buf_len++] = 0;
        sha1Transform(c, c.buf);
        c.buf_len = 0;
    }
    while (c.buf_len < 56) c.buf[c.buf_len++] = 0;
    // bit_len big-endian
    for (int i = 7; i >= 0; --i) {
        c.buf[c.buf_len++] = static_cast<uint8_t>((c.bit_len >> (i * 8)) & 0xFF);
    }
    sha1Transform(c, c.buf);
    for (int i = 0; i < 5; ++i) {
        out[i * 4]     = static_cast<uint8_t>((c.h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((c.h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((c.h[i] >> 8)  & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>( c.h[i]        & 0xFF);
    }
}

// ---- Helpers ----------------------------------------------------------------

std::string extractTagValue(const std::string& xml, const std::string& local_name) {
    // 宽松匹配：忽略命名空间前缀和属性。regex 编译一次，static const 线程安全。
    // 模式：<(?:[^>]*:)?name[^>]*>(.*?)</(?:[^>]*:)?name>
    const std::string pat =
        "<(?:[^>]*:)?" + local_name + "[^>]*>([^<]*)</(?:[^>]*:)?" + local_name + ">";
    std::regex re(pat, std::regex::icase);
    std::smatch m;
    if (std::regex_search(xml, m, re)) return m[1].str();
    return {};
}

bool parseIsoUtc(const std::string& s, std::chrono::system_clock::time_point* tp) {
    // 仅支持 2026-04-22T00:00:00Z 或 2026-04-22T00:00:00.123Z 形式
    static const std::regex re(R"((\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.\d+)?Z)");
    std::smatch m;
    if (!std::regex_search(s, m, re)) return false;
    std::tm tm{};
    tm.tm_year = std::stoi(m[1]) - 1900;
    tm.tm_mon  = std::stoi(m[2]) - 1;
    tm.tm_mday = std::stoi(m[3]);
    tm.tm_hour = std::stoi(m[4]);
    tm.tm_min  = std::stoi(m[5]);
    tm.tm_sec  = std::stoi(m[6]);
#ifdef _WIN32
    const time_t t = _mkgmtime(&tm);
#else
    const time_t t = timegm(&tm);
#endif
    if (t == -1) return false;
    *tp = std::chrono::system_clock::from_time_t(t);
    return true;
}

}  // namespace

std::string sha1Raw(const uint8_t* data, size_t len) {
    Sha1Ctx c;
    sha1Init(c);
    sha1Update(c, data, len);
    uint8_t out[20];
    sha1Final(c, out);
    return std::string(reinterpret_cast<char*>(out), 20);
}

WsseAuthenticator::WsseAuthenticator() = default;

void WsseAuthenticator::setCredentials(const std::string& user, const std::string& pass) {
    std::lock_guard<std::mutex> lock(mutex_);
    expected_user_ = user;
    expected_pass_ = pass;
    seen_tokens_.clear();
}

bool WsseAuthenticator::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !expected_user_.empty();
}

WsseAuthenticator::Result WsseAuthenticator::verify(const std::string& soap_xml) {
    Result r;
    std::string exp_user, exp_pass;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exp_user = expected_user_;
        exp_pass = expected_pass_;
    }
    if (exp_user.empty()) {
        r.ok = true;
        return r;
    }

    const std::string username  = extractTagValue(soap_xml, "Username");
    const std::string password  = extractTagValue(soap_xml, "Password");
    const std::string nonce_b64 = extractTagValue(soap_xml, "Nonce");
    const std::string created   = extractTagValue(soap_xml, "Created");

    if (username.empty() || password.empty() || nonce_b64.empty() || created.empty()) {
        r.failure_reason = "missing UsernameToken fields";
        return r;
    }
    if (username != exp_user) {
        r.failure_reason = "username mismatch";
        return r;
    }

    // 校验 Created 时间窗 ±5 分钟（部分客户端时钟偏差较大，保守放宽）
    std::chrono::system_clock::time_point created_tp;
    if (!parseIsoUtc(created, &created_tp)) {
        r.failure_reason = "created timestamp parse failed";
        return r;
    }
    const auto now = std::chrono::system_clock::now();
    const auto dt = now > created_tp ? (now - created_tp) : (created_tp - now);
    if (dt > std::chrono::minutes(5)) {
        r.failure_reason = "created timestamp outside ±5min window";
        return r;
    }

    // 防重放：nonce+created 组合若已见过，拒绝
    const std::string token_key = nonce_b64 + "|" + created;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (seen_tokens_.count(token_key)) {
            r.failure_reason = "replay detected";
            return r;
        }
        if (seen_tokens_.size() >= kSeenLimit) {
            // 简单淘汰策略：超过上限就整个清空（保守，不追求复杂 LRU）
            seen_tokens_.clear();
        }
        seen_tokens_.insert(token_key);
    }

    // 计算期望摘要 = base64( SHA1( nonce_raw + created + password ) )
    const std::vector<uint8_t> nonce_raw = base64Decode(nonce_b64);
    std::vector<uint8_t> input;
    input.reserve(nonce_raw.size() + created.size() + exp_pass.size());
    input.insert(input.end(), nonce_raw.begin(), nonce_raw.end());
    input.insert(input.end(), created.begin(), created.end());
    input.insert(input.end(), exp_pass.begin(), exp_pass.end());

    const std::string digest_raw = sha1Raw(input.data(), input.size());
    const std::string digest_b64 = base64Encode(
        reinterpret_cast<const uint8_t*>(digest_raw.data()), digest_raw.size());

    if (digest_b64 != password) {
        r.failure_reason = "password digest mismatch";
        return r;
    }

    r.ok = true;
    r.username = username;
    return r;
}

}  // namespace rtsp
