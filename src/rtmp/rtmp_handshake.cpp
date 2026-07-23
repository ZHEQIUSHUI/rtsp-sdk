#include "rtmp_handshake.h"

#include <rtsp-common/common.h>

#include <chrono>
#include <cstring>
#include <random>

namespace rtsp {

namespace {

constexpr size_t kHandshakeSize = 1536;

bool recvExact(Socket& s, uint8_t* out, size_t n, int deadline_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    size_t off = 0;
    while (off < n) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remain < 1) remain = 1;
        const int poll_to = remain < 200 ? remain : 200;
        // 先等可读，把「超时」与「硬错误」分开：waitReadable 返回 0=超时（继续到
        // deadline）、-1=POLLERR/POLLHUP/POLLNVAL（对端断，立即退出）。旧写法把 recv<0
        // 一律当超时重试——Linux 上 RST 后 recv 先 -1 再 0，靠 r==0 退出还算快；但那是
        // 平台特定行为，在不保证「ECONNRESET 后 recv 返回 0」的系统上会 poll 立即可读+
        // recv 持续 -1 而忙等。显式判 POLLERR/POLLHUP 更稳、也少一次无谓迭代。
        const int wr = s.waitReadable(poll_to);
        if (wr < 0) return false;   // 硬错误/对端挂断，立即退出
        if (wr == 0) continue;      // 超时，回到循环顶重新算 deadline
        const ssize_t r = s.recv(out + off, n - off, 0);
        if (r > 0) {
            off += static_cast<size_t>(r);
        } else {
            return false;   // r==0 对端关闭；r<0 真错误。均立即失败
        }
    }
    return true;
}

}  // namespace

bool rtmpSimpleHandshakeClient(Socket& s, int timeout_ms) {
    if (!s.isValid()) return false;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed_ms = [&]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    };
    auto remain_ms = [&]() {
        return std::max(1, timeout_ms - elapsed_ms());
    };

    // 构造 C0 + C1
    uint8_t c0c1[1 + kHandshakeSize];
    c0c1[0] = 0x03;  // C0: version=3
    // C1: 4B timestamp = 0；4B zero（spec 要求前 4 字节为 0 以表示 simple handshake）；
    // 之后 1528 字节随机。
    std::memset(c0c1 + 1, 0, 8);
    std::random_device rd;
    std::mt19937_64 rng;
    std::seed_seq seq{rd(), rd(), rd(), rd()};
    rng.seed(seq);
    for (size_t i = 9; i < 1 + kHandshakeSize; i += 8) {
        uint64_t v = rng();
        const size_t room = std::min<size_t>(8, (1 + kHandshakeSize) - i);
        std::memcpy(c0c1 + i, &v, room);
    }

    // 发 C0+C1（一次性写出更高效，也方便部分 server 的状态机）
    if (s.sendAll(c0c1, sizeof(c0c1), remain_ms()) != static_cast<ssize_t>(sizeof(c0c1))) {
        return false;
    }

    // 收 S0+S1
    uint8_t s0;
    if (!recvExact(s, &s0, 1, remain_ms())) return false;
    if (s0 != 0x03) {
        RTSP_LOG_WARNING("RTMP handshake: unexpected S0 version");
        return false;
    }
    uint8_t s1[kHandshakeSize];
    if (!recvExact(s, s1, kHandshakeSize, remain_ms())) return false;

    // 发 C2 = echo of S1
    if (s.sendAll(s1, kHandshakeSize, remain_ms()) != static_cast<ssize_t>(kHandshakeSize)) {
        return false;
    }

    // 收 S2 = echo of C1（严格 client 应校验，但部分 server 略作改动；这里只消费掉）
    uint8_t s2[kHandshakeSize];
    if (!recvExact(s, s2, kHandshakeSize, remain_ms())) return false;

    return true;
}

}  // namespace rtsp
