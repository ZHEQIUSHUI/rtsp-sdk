#pragma once

// WS-Security UsernameToken PasswordDigest 鉴权，ONVIF 设备默认模式。
//
// 格式（SOAP Header 里）：
//   <wsse:Security>
//     <wsse:UsernameToken>
//       <wsse:Username>admin</wsse:Username>
//       <wsse:Password Type="...#PasswordDigest">$base64(SHA1(Nonce+Created+Password))</wsse:Password>
//       <wsse:Nonce EncodingType="...#Base64Binary">$base64(random bytes)</wsse:Nonce>
//       <wsu:Created>2026-04-22T00:00:00Z</wsu:Created>
//     </wsse:UsernameToken>
//   </wsse:Security>
//
// 本模块职责：
//   1. 从 SOAP header 文本里解析上述 4 字段（不做严格 XML 解析，字符串匹配即可）
//   2. 校验 PasswordDigest = base64(SHA1(nonce_bytes + created_str + password))
//   3. 校验 Created 在可接受时间窗内（±5 分钟），防重放
//   4. 记录已用过的 (nonce, created) 组合，防重放

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

namespace rtsp {

class WsseAuthenticator {
public:
    struct Result {
        bool ok = false;
        std::string username;          // 成功时填充
        std::string failure_reason;    // 失败时填充，仅调试用
    };

    WsseAuthenticator();

    // 设置期望的 user/pass；清空即禁用鉴权（任何请求都通过）
    void setCredentials(const std::string& user, const std::string& pass);
    bool enabled() const;

    // 从完整 SOAP 请求 XML 中解析并校验 UsernameToken。
    // 若未启用，总是返回 ok=true。
    Result verify(const std::string& soap_xml);

private:
    mutable std::mutex mutex_;
    std::string expected_user_;
    std::string expected_pass_;
    // 已见过的 (nonce + created) 组合，防重放。大小有界，超出则淘汰老的。
    std::unordered_set<std::string> seen_tokens_;
    static constexpr size_t kSeenLimit = 1024;
};

// 工具：SHA1 和 Base64。ONVIF 摘要计算的最小必需集合。
// Base64 已在 rtsp-common/common.h 提供，这里只需补 SHA1。
std::string sha1Raw(const uint8_t* data, size_t len);   // 返回 20 字节 raw

}  // namespace rtsp
