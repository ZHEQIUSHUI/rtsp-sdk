#include <rtsp-common/common.h>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

namespace rtsp {

// Base64编码表
static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t size) {
    std::string encoded;
    encoded.reserve(((size + 2) / 3) * 4);
    
    for (size_t i = 0; i < size; i += 3) {
        uint32_t octet_a = i < size ? data[i] : 0;
        uint32_t octet_b = (i + 1) < size ? data[i + 1] : 0;
        uint32_t octet_c = (i + 2) < size ? data[i + 2] : 0;
        
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
        
        encoded.push_back(base64_chars[(triple >> 18) & 0x3F]);
        encoded.push_back(base64_chars[(triple >> 12) & 0x3F]);
        encoded.push_back((i + 1) < size ? base64_chars[(triple >> 6) & 0x3F] : '=');
        encoded.push_back((i + 2) < size ? base64_chars[triple & 0x3F] : '=');
    }
    
    return encoded;
}

std::vector<uint8_t> base64Decode(const std::string& str) {
    std::vector<uint8_t> decoded;

    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    int in_len = static_cast<int>(str.size());
    int i = 0, j = 0;
    uint8_t char_array_4[4], char_array_3[3];

    while (in_len-- && (str[i] != '=')) {
        int v = decode_char(str[i]);
        if (v < 0) {
            break;
        }
        char_array_4[j++] = static_cast<uint8_t>(v);
        i++;
        if (j == 4) {
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0x0F) << 4) + ((char_array_4[2] & 0x3C) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x03) << 6) + char_array_4[3];

            for (j = 0; j < 3; j++) {
                decoded.push_back(char_array_3[j]);
            }
            j = 0;
        }
    }

    if (j) {
        for (int k = j; k < 4; k++) {
            char_array_4[k] = 0;
        }

        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0x0F) << 4) + ((char_array_4[2] & 0x3C) >> 2);

        for (int k = 0; k < (j - 1); k++) {
            decoded.push_back(char_array_3[k]);
        }
    }

    return decoded;
}

namespace {

inline uint32_t leftRotate(uint32_t x, uint32_t c) {
    return (x << c) | (x >> (32 - c));
}

} // namespace

std::string md5Hex(const std::string& input) {
    static const uint32_t s[] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    static const uint32_t k[] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };

    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8;

    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 0; i < 8; ++i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (8 * i)) & 0xFF));
    }

    uint32_t a0 = 0x67452301;
    uint32_t b0 = 0xefcdab89;
    uint32_t c0 = 0x98badcfe;
    uint32_t d0 = 0x10325476;

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<uint32_t>(msg[offset + i * 4]) |
                   (static_cast<uint32_t>(msg[offset + i * 4 + 1]) << 8) |
                   (static_cast<uint32_t>(msg[offset + i * 4 + 2]) << 16) |
                   (static_cast<uint32_t>(msg[offset + i * 4 + 3]) << 24);
        }

        uint32_t a = a0, b = b0, c = c0, d = d0;

        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t f = 0, g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) % 16;
            }
            uint32_t tmp = d;
            d = c;
            c = b;
            b = b + leftRotate(a + f + k[i] + m[g], s[i]);
            a = tmp;
        }

        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    uint8_t digest[16];
    uint32_t h[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i) {
        digest[i * 4] = static_cast<uint8_t>(h[i] & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
    }

    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : digest) {
        out.push_back(hex[(b >> 4) & 0x0F]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

// 日志系统
static LogCallback g_log_callback = nullptr;
static LogConfig g_log_config{};
static std::mutex g_log_mutex;

namespace {

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARNING";
        case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

std::string formatTimestamp(bool use_utc_time) {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    if (use_utc_time) {
        gmtime_s(&tm_buf, &tt);
    } else {
        localtime_s(&tm_buf, &tt);
    }
#else
    if (use_utc_time) {
        gmtime_r(&tt, &tm_buf);
    } else {
        localtime_r(&tt, &tm_buf);
    }
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << ms.count();
    if (use_utc_time) {
        oss << "Z";
    }
    return oss.str();
}

std::string escapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (char c : input) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string buildLogLine(LogLevel level, const std::string& msg, const LogConfig& cfg) {
    const std::string ts = formatTimestamp(cfg.use_utc_time);
    if (cfg.format == LogFormat::Json) {
        std::ostringstream oss;
        oss << "{\"ts\":\"" << ts << "\",\"level\":\"" << logLevelName(level) << "\"";
        if (cfg.include_thread_id) {
            std::ostringstream tid;
            tid << std::this_thread::get_id();
            oss << ",\"thread\":\"" << tid.str() << "\"";
        }
        oss << ",\"msg\":\"" << escapeJson(msg) << "\"}";
        return oss.str();
    }

    std::ostringstream oss;
    oss << "[" << ts << "] "
        << "[" << logLevelName(level) << "]";
    if (cfg.include_thread_id) {
        std::ostringstream tid;
        tid << std::this_thread::get_id();
        oss << " [T:" << tid.str() << "]";
    }
    oss << " " << msg;
    return oss.str();
}

} // namespace

void setLogConfig(const LogConfig& config) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_config = config;
}

LogConfig getLogConfig() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_log_config;
}

void setLogCallback(LogCallback callback) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_callback = callback;
}

void log(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (static_cast<int>(level) < static_cast<int>(g_log_config.min_level)) {
        return;
    }
    if (g_log_callback) {
        g_log_callback(level, msg);
    } else {
        const std::string line = buildLogLine(level, msg, g_log_config);
        fprintf(stderr, "%s\n", line.c_str());
    }
}

} // namespace rtsp
