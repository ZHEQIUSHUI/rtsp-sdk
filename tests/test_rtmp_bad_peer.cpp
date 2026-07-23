// RTMP 坏 peer 健壮性冒烟测试。
//
// 验证的属性：对端在 client 等待接收时用 RST 断开，RtmpPublisher::open 必须在
// 有限时间内失败（这里 <2500ms，远小于 5000ms 的 handshake_timeout），且不崩溃、
// 返回错误——覆盖握手 recvExact 与 doConnect 的 waitForCommandResult 两条路径。
//
// 诚实说明它「不」是什么：这不是 RTMP-SPIN 的专项回归。实测在 Linux 上，对端 RST
// 后 recv 第一次返回 -1/ECONNRESET、紧接着第二次返回 0，所以「修复前」的代码遇 RST
// 也会在 2 次迭代内（经 recv==0 的 "peer closed" 分支）快速失败，并不会忙等满 timeout。
// 因此本测试在 Linux 上无法区分 waitReadable 修复的前后——两者都快速失败。
//
// 它仍有价值：①守卫未来把 sendAll 改回裸 send、或把等待循环改成无 deadline 之类
// 「真 hang」回归；②在 ECONNRESET 后 recv 不返回 0 的平台（部分 BSD/macOS 语义）上，
// 修复后的 waitReadable 显式处理 POLLERR/POLLHUP 才真正避免忙等，此测试会体现差异。
//
// mock server 用裸 POSIX socket（而非被测 Socket 类）：TCP 规定 close 一个"接收缓冲
// 仍有未读数据"的 socket 会发 RST 而非 FIN（RFC 1122 §4.2.2.13），裸 socket 可靠复现
// 真实对端（mediamtx/CDN）RST 的行为。Windows 下跳过（posix-only）。

#include <rtsp-rtmp/rtsp-rtmp.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <atomic>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace rtsp;
using namespace std::chrono_literals;

#ifndef _WIN32
namespace {

class BadPeer {
public:
    enum class Mode { ResetDuringHandshake, ResetDuringCommand };

    bool start(Mode m) {
        mode_ = m;
        lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lfd_ < 0) return false;
        int one = 1;
        setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;                                    // 动态端口，避免撞车
        if (::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) return false;
        socklen_t al = sizeof(a);
        if (::getsockname(lfd_, reinterpret_cast<sockaddr*>(&a), &al) < 0) return false;
        port_ = ntohs(a.sin_port);
        if (::listen(lfd_, 1) < 0) return false;
        running_.store(true);
        th_ = std::thread([this] { runOnce(); });
        return true;
    }

    uint16_t port() const { return port_; }

    void stop() {
        running_.store(false);
        if (th_.joinable()) th_.join();               // runOnce 自我退出后 lfd_ 不再被读
        if (lfd_ >= 0) { ::close(lfd_); lfd_ = -1; }   // join 之后关闭，无跨线程竞争
    }

private:
    static bool recvN(int fd, uint8_t* out, size_t n) {
        size_t off = 0;
        while (off < n) {
            const ssize_t r = ::recv(fd, out + off, n - off, 0);
            if (r <= 0) return false;
            off += static_cast<size_t>(r);
        }
        return true;
    }

    // 服务端 RTMP 简单握手：收 C0C1 → 发 S0S1S2 → 收 C2
    static bool handshake(int fd) {
        std::vector<uint8_t> c0c1(1 + 1536);
        if (!recvN(fd, c0c1.data(), c0c1.size())) return false;
        if (c0c1[0] != 0x03) return false;
        std::vector<uint8_t> s(1 + 1536 + 1536, 0);
        s[0] = 0x03;                                        // S0 version
        std::memcpy(s.data() + 1 + 1536, c0c1.data() + 1, 1536);  // S2 = C1（echo）
        if (::send(fd, s.data(), s.size(), 0) != static_cast<ssize_t>(s.size())) return false;
        std::vector<uint8_t> c2(1536);
        if (!recvN(fd, c2.data(), c2.size())) return false;
        return true;
    }

    void runOnce() {
        // 用 poll 轮询 accept，受 running_ + 5s deadline 约束自我退出——这样 stop() 只需
        // running_.store(false) 再 join，不必跨线程碰 lfd_，避免与本线程的数据竞争。
        int fd = -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (running_.load() && std::chrono::steady_clock::now() < deadline) {
            pollfd p{};
            p.fd = lfd_;
            p.events = POLLIN;
            if (::poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
                fd = ::accept(lfd_, nullptr, nullptr);
                break;
            }
        }
        if (fd < 0) return;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // ResetDuringHandshake：不发 S0S1S2，client 阻塞在握手 recvExact 等 S0。
        // ResetDuringCommand：完成握手，client 发 window-ack + connect 后阻塞在
        //   waitForCommandResult 等 _result。
        if (mode_ == Mode::ResetDuringCommand) {
            if (!handshake(fd)) { ::close(fd); return; }
        }
        // 此后 server 完全不读 client 发来的数据；稍等让 client 阻塞在 recv 上，
        // 再 close —— 接收缓冲满是未读数据 → RST，client 的 recv 拿到 -1。
        std::this_thread::sleep_for(200ms);
        ::close(fd);
    }

    int lfd_ = -1;
    std::thread th_;
    std::atomic<bool> running_{false};
    uint16_t port_ = 0;
    Mode mode_ = Mode::ResetDuringHandshake;
};

int runCase(BadPeer::Mode mode, const char* name) {
    BadPeer peer;
    if (!peer.start(mode)) {
        std::cerr << "SKIP " << name << ": cannot bind mock server\n";
        return 0;   // 环境不允许，跳过（不算失败）
    }

    RtmpPublisher pub;
    RtmpPublishConfig cfg;
    cfg.connect_timeout_ms   = 2000;
    cfg.handshake_timeout_ms = 5000;   // 大超时：修复前会忙等满这个
    cfg.send_timeout_ms      = 2000;
    pub.setConfig(cfg);

    RtmpPublishMediaInfo media;
    media.codec  = CodecType::H264;
    media.width  = 320;
    media.height = 240;
    media.fps    = 25;

    const std::string url = "rtmp://127.0.0.1:" + std::to_string(peer.port()) + "/live/key";
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = pub.open(url, media);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    peer.stop();

    std::cout << name << ": open=" << (ok ? "true" : "false")
              << " elapsed=" << ms << "ms err=\"" << pub.getLastError() << "\"\n";

    int fail = 0;
    if (ok) {
        std::cerr << "  FAIL: open() should fail against a peer that RSTs the connection\n";
        fail = 1;
    }
    if (ms >= 2500) {
        std::cerr << "  FAIL: open() took " << ms << "ms (>=2500) against a dead peer — it must "
                     "fail well under the 5000ms timeout, not hang\n";
        fail = 1;
    }
    return fail;
}

}  // namespace
#endif  // !_WIN32

int main() {
#ifdef _WIN32
    std::cout << "SKIP: rtmp bad-peer test is posix-only\n";
    return 0;
#else
    int fail = 0;
    fail += runCase(BadPeer::Mode::ResetDuringHandshake, "reset-during-handshake (recvExact)");
    fail += runCase(BadPeer::Mode::ResetDuringCommand,   "reset-during-command (waitForCommandResult)");
    if (fail == 0) std::cout << "all rtmp bad-peer fast-fail tests passed\n";
    return fail ? 1 : 0;
#endif
}
