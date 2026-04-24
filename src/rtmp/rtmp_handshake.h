#pragma once

// RTMP Simple Handshake（C0-C2 / S0-S2）。
// 参考：Adobe RTMP Specification, 1.0 §5.2.
//
// 版本：C0/S0 固定 0x03（RTMP-plain，非加密）。
// C1/S1：1536 字节 = 4B timestamp + 4B zero + 1528B 任意字节。
// C2/S2：1536 字节 = 对方 S1/C1 的 echo（严格按序列字节拷回）。
//
// 这一实现做的是 "simple" 变体。绝大多数服务器（mediamtx / SRS / nginx-rtmp /
// 国内主流 CDN）都接受。YouTube / Twitch 要求 "complex handshake"（带 HMAC-SHA256
// 校验），暂不支持。

#include <rtsp-common/socket.h>
#include <cstdint>

namespace rtsp {

// 阻塞式完成 C0..C2 + 读完 S0..S2。返回 true = 成功；timeout_ms 总耗时上限。
bool rtmpSimpleHandshakeClient(Socket& s, int timeout_ms);

}  // namespace rtsp
