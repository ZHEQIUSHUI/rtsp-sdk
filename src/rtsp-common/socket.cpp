// NOMINMAX 必须在任何 Windows 头之前定义，防止 windows.h 把 min/max 作为宏
// 污染，从而破坏 std::min/std::max 以及 (std::numeric_limits<T>::max)() 等
// 标准 API 调用。MSVC 下的 winsock2.h 会间接拉 windows.h。
#if defined(_WIN32) && !defined(NOMINMAX)
    #define NOMINMAX
#endif

#include <rtsp-common/socket.h>
#include <rtsp-common/common.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <vector>
#include <thread>
#include <atomic>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    // 防御性：某些 Windows SDK 版本即便 NOMINMAX 也会在特定路径引入宏
    #ifdef min
        #undef min
    #endif
    #ifdef max
        #undef max
    #endif
    typedef int socklen_t;
    using pollfd = WSAPOLLFD;
    #ifndef POLLIN
        #define POLLIN POLLRDNORM
    #endif
    #ifndef POLLOUT
        #define POLLOUT POLLWRNORM
    #endif
    static int poll(pollfd* fds, size_t nfds, int timeout_ms) {
        return WSAPoll(fds, static_cast<ULONG>(nfds), timeout_ms);
    }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>
#endif

namespace rtsp {

// 初始化网络库（Windows需要）
class NetworkInit {
public:
    NetworkInit() {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    }
    ~NetworkInit() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

static NetworkInit g_network_init;

// Socket实现
class Socket::Impl {
public:
    // fd_ 可被多个线程并发 close（Server stop 与连接线程自清理），
    // 用 atomic<int> + compare_exchange 保证只关一次、避免 TSan 数据竞争。
    std::atomic<int> fd_{-1};
    std::string local_ip_;
    uint16_t local_port_ = 0;
    std::string peer_ip_;
    uint16_t peer_port_ = 0;

    Impl() = default;
    explicit Impl(int fd) : fd_(fd) {
        if (fd >= 0) {
            updateLocalAddr();
            updatePeerAddr();
        }
    }

    ~Impl() {
        close();
    }

    void close() {
        int cur = fd_.load(std::memory_order_acquire);
        while (cur >= 0) {
            if (fd_.compare_exchange_weak(cur, -1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
#ifdef _WIN32
                ::closesocket(cur);
#else
                ::close(cur);
#endif
                return;
            }
            // cur 已更新为当前值；若仍 >=0 循环继续
        }
    }

    void updateLocalAddr() {
        if (fd_ < 0) return;
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        if (getsockname(fd_, (struct sockaddr*)&addr, &len) == 0) {
            char ip_str[INET6_ADDRSTRLEN];
            if (addr.ss_family == AF_INET) {
                struct sockaddr_in* sin = (struct sockaddr_in*)&addr;
                inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
                local_port_ = ntohs(sin->sin_port);
            } else {
                struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&addr;
                inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, sizeof(ip_str));
                local_port_ = ntohs(sin6->sin6_port);
            }
            local_ip_ = ip_str;
        }
    }

    void updatePeerAddr() {
        if (fd_ < 0) return;
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        if (getpeername(fd_, (struct sockaddr*)&addr, &len) == 0) {
            char ip_str[INET6_ADDRSTRLEN];
            if (addr.ss_family == AF_INET) {
                struct sockaddr_in* sin = (struct sockaddr_in*)&addr;
                inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
                peer_port_ = ntohs(sin->sin_port);
            } else {
                struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&addr;
                inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, sizeof(ip_str));
                peer_port_ = ntohs(sin6->sin6_port);
            }
            peer_ip_ = ip_str;
        }
    }
};

Socket::Socket() : impl_(std::make_unique<Impl>()) {}
Socket::Socket(int fd) : impl_(std::make_unique<Impl>(fd)) {}
Socket::~Socket() = default;

Socket::Socket(Socket&& other) noexcept = default;
Socket& Socket::operator=(Socket&& other) noexcept = default;

bool Socket::bind(const std::string& ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;

    impl_->fd_ = fd;
    if (!setReuseAddr(true)) {
        impl_->close();
        return false;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        impl_->close();
        return false;
    }

    impl_->updateLocalAddr();
    return true;
}

bool Socket::listen(int backlog) {
    if (impl_->fd_ < 0) return false;
    return ::listen(impl_->fd_, backlog) == 0;
}

std::unique_ptr<Socket> Socket::accept() {
    if (impl_->fd_ < 0) return nullptr;

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int client_fd = ::accept(impl_->fd_, (struct sockaddr*)&addr, &len);

    if (client_fd < 0) return nullptr;

    auto sock = std::make_unique<Socket>(client_fd);
    // RTSP 控制通道小请求多、RTP-over-TCP 小包多：禁用 Nagle 以减少延迟和抖动
    sock->setTcpNoDelay(true);
    return sock;
}

bool Socket::connect(const std::string& ip, uint16_t port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;

    impl_->fd_ = fd;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (timeout_ms > 0) {
        setNonBlocking(true);
        int res = ::connect(fd, (struct sockaddr*)&addr, sizeof(addr));
        if (res < 0) {
#ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
#else
            if (errno != EINPROGRESS) {
#endif
                impl_->close();
                return false;
            }
        }

        // 等待连接完成
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        
        res = poll(&pfd, 1, timeout_ms);
        if (res <= 0) {
            impl_->close();
            return false;
        }

        int so_error;
        socklen_t len = sizeof(so_error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error != 0) {
            impl_->close();
            return false;
        }

        setNonBlocking(false);
    } else {
        if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            impl_->close();
            return false;
        }
    }

    impl_->updateLocalAddr();
    impl_->updatePeerAddr();
    // TCP client socket：也禁用 Nagle
    int opt = 1;
    setsockopt(impl_->fd_, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    return true;
}

bool Socket::bindUdp(const std::string& ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;

    impl_->fd_ = fd;
    if (!setReuseAddr(true)) {
        impl_->close();
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        impl_->close();
        return false;
    }

    impl_->updateLocalAddr();
    return true;
}

ssize_t Socket::sendTo(const uint8_t* data, size_t size, const std::string& ip, uint16_t port) {
    if (impl_->fd_ < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    return sendto(impl_->fd_, (const char*)data, size, 0, 
                  (struct sockaddr*)&addr, sizeof(addr));
}

ssize_t Socket::recvFrom(uint8_t* buffer, size_t size, std::string& from_ip, uint16_t& from_port) {
    if (impl_->fd_ < 0) return -1;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    
    ssize_t ret = recvfrom(impl_->fd_, (char*)buffer, size, 0,
                           (struct sockaddr*)&addr, &addr_len);
    
    if (ret > 0) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
        from_ip = ip_str;
        from_port = ntohs(addr.sin_port);
    }

    return ret;
}

ssize_t Socket::send(const uint8_t* data, size_t size) {
    if (impl_->fd_ < 0) return -1;
#ifdef _WIN32
    return ::send(impl_->fd_, (const char*)data, size, 0);
#else
    return ::send(impl_->fd_, data, size, MSG_NOSIGNAL);
#endif
}

ssize_t Socket::sendAll(const uint8_t* data, size_t size, int timeout_ms) {
    if (impl_->fd_ < 0) return -1;
    if (size == 0) return 0;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    size_t off = 0;
    while (off < size) {
        pollfd pfd;
        pfd.fd = impl_->fd_;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        const auto now = std::chrono::steady_clock::now();
        int remain_ms = 0;
        if (timeout_ms <= 0) {
            remain_ms = 0;
        } else if (now < deadline) {
            remain_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (remain_ms <= 0) remain_ms = 1;
        } else {
            return off > 0 ? static_cast<ssize_t>(off) : -1;
        }

        int pr = poll(&pfd, 1, remain_ms);
        if (pr == 0) {
            // timeout: 如果已经发了一些，上层可能不想部分写，但保留一致语义：
            // 返回 -1 表示未在期限内完成发送；调用方据此判定超时并关闭连接。
            return off > 0 ? static_cast<ssize_t>(off) : -1;
        }
        if (pr < 0) {
            return -1;
        }
        if (!(pfd.revents & POLLOUT)) {
            // POLLHUP/POLLERR
            return -1;
        }

#ifdef _WIN32
        ssize_t r = ::send(impl_->fd_, reinterpret_cast<const char*>(data + off),
                           static_cast<int>(size - off), 0);
#else
        ssize_t r = ::send(impl_->fd_, data + off, size - off, MSG_NOSIGNAL | MSG_DONTWAIT);
#endif
        if (r > 0) {
            off += static_cast<size_t>(r);
            continue;
        }
        if (r == 0) {
            return off > 0 ? static_cast<ssize_t>(off) : -1;
        }
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) continue;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
        return -1;
    }
    return static_cast<ssize_t>(off);
}

ssize_t Socket::recv(uint8_t* buffer, size_t size, int timeout_ms) {
    if (impl_->fd_ < 0) return -1;

    if (timeout_ms >= 0) {
        pollfd pfd;
        pfd.fd = impl_->fd_;
        pfd.events = POLLIN;
        
        int res = poll(&pfd, 1, timeout_ms);
        if (res == 0) {
            // Timeout. Return -1 so callers can treat this as "no data yet" instead of
            // "connection closed" (recv(2) would return 0 only when peer closed).
#ifndef _WIN32
            errno = EAGAIN;
#endif
            return -1;
        }
        if (res < 0) {
            return -1;
        }
    }

    return ::recv(impl_->fd_, (char*)buffer, size, 0);
}

void Socket::close() {
    impl_->close();
}

bool Socket::shutdownReadWrite() {
    if (impl_->fd_ < 0) return false;
#ifdef _WIN32
    return ::shutdown(impl_->fd_, SD_BOTH) == 0;
#else
    return ::shutdown(impl_->fd_, SHUT_RDWR) == 0;
#endif
}

bool Socket::setNonBlocking(bool non_blocking) {
    if (impl_->fd_ < 0) return false;

#ifdef _WIN32
    u_long mode = non_blocking ? 1 : 0;
    return ioctlsocket(impl_->fd_, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(impl_->fd_, F_GETFL, 0);
    if (flags < 0) return false;
    flags = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(impl_->fd_, F_SETFL, flags) == 0;
#endif
}

bool Socket::setReuseAddr(bool reuse) {
    if (impl_->fd_ < 0) return false;
    int opt = reuse ? 1 : 0;
    return setsockopt(impl_->fd_, SOL_SOCKET, SO_REUSEADDR, 
                      (const char*)&opt, sizeof(opt)) == 0;
}

bool Socket::setSendBufferSize(int size) {
    if (impl_->fd_ < 0) return false;
    return setsockopt(impl_->fd_, SOL_SOCKET, SO_SNDBUF,
                      (const char*)&size, sizeof(size)) == 0;
}

bool Socket::setRecvBufferSize(int size) {
    if (impl_->fd_ < 0) return false;
    return setsockopt(impl_->fd_, SOL_SOCKET, SO_RCVBUF,
                      (const char*)&size, sizeof(size)) == 0;
}

bool Socket::setTcpNoDelay(bool enable) {
    if (impl_->fd_ < 0) return false;
    int opt = enable ? 1 : 0;
    // TCP_NODELAY on UDP sockets returns ENOPROTOOPT; callers should only use on TCP.
    return setsockopt(impl_->fd_, IPPROTO_TCP, TCP_NODELAY,
                      (const char*)&opt, sizeof(opt)) == 0;
}

int Socket::waitReadable(int timeout_ms) const {
    if (impl_->fd_ < 0) return -1;
    pollfd pfd;
    pfd.fd = impl_->fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return (pfd.revents & POLLIN) ? 1 : 0;
}

bool Socket::setSendTimeout(int timeout_ms) {
    if (impl_->fd_ < 0) return false;
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms);
    return setsockopt(impl_->fd_, SOL_SOCKET, SO_SNDTIMEO,
                      reinterpret_cast<const char*>(&tv), sizeof(tv)) == 0;
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(impl_->fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool Socket::isValid() const {
    return impl_->fd_ >= 0;
}

int Socket::getFd() const {
    return impl_->fd_;
}

std::string Socket::getLocalIp() const {
    return impl_->local_ip_;
}

uint16_t Socket::getLocalPort() const {
    return impl_->local_port_;
}

std::string Socket::getPeerIp() const {
    return impl_->peer_ip_;
}

uint16_t Socket::getPeerPort() const {
    return impl_->peer_port_;
}

// ----------------------------------------------------------------
// 公共工具：读完整 RTSP 消息
// ----------------------------------------------------------------
bool recvRtspMessage(Socket& socket, std::string* out, int timeout_ms) {
    if (out == nullptr) return false;
    out->clear();

    const auto timeout = std::chrono::milliseconds(std::max(0, timeout_ms));
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    auto parseContentLength = [](const std::string& headers, std::size_t* cl) -> bool {
        // 手写解析，避免拉 <regex>
        size_t pos = 0;
        const std::string needle_lc = "content-length:";
        // case-insensitive search
        std::string headers_lc;
        headers_lc.reserve(headers.size());
        for (char c : headers) headers_lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        pos = headers_lc.find(needle_lc);
        if (pos == std::string::npos) return false;
        pos += needle_lc.size();
        // skip spaces
        while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) end = headers.size();
        std::string val = headers.substr(pos, end - pos);
        // trim trailing ws
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        try {
            unsigned long long v = std::stoull(val);
            *cl = static_cast<std::size_t>(v);
            return true;
        } catch (...) {
            return false;
        }
    };

    std::size_t expected_total = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const int poll_timeout_ms = std::min(200, std::max(1, remain));

        uint8_t buffer[4096];
        ssize_t len = socket.recv(buffer, sizeof(buffer), poll_timeout_ms);
        if (len > 0) {
            out->append(reinterpret_cast<char*>(buffer), static_cast<size_t>(len));
            if (expected_total == 0) {
                const auto header_end = out->find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const std::size_t header_size = header_end + 4;
                    std::size_t cl = 0;
                    if (parseContentLength(out->substr(0, header_size), &cl)) {
                        expected_total = header_size + cl;
                    } else {
                        expected_total = header_size;
                    }
                }
            }
            if (expected_total != 0 && out->size() >= expected_total) {
                // 丢弃尾部 pipeline 的额外字节，保持单次调用单条消息的语义
                out->resize(expected_total);
                return true;
            }
        } else if (len == 0) {
            // peer closed
            return false;
        } else {
            // timeout / transient; loop
            continue;
        }
    }
    return false;
}

// Selector实现
class Selector::Impl {
public:
    std::vector<pollfd> fds_;
    std::vector<int> fd_indices_;  // fd到索引的映射（简单起见，使用线性查找）

    pollfd* findPfd(int fd) {
        for (auto& pfd : fds_) {
            if (pfd.fd == fd) return &pfd;
        }
        return nullptr;
    }
};

Selector::Selector() : impl_(std::make_unique<Impl>()) {}
Selector::~Selector() = default;

void Selector::addRead(int fd) {
    pollfd* pfd = impl_->findPfd(fd);
    if (pfd) {
        pfd->events |= POLLIN;
    } else {
        pollfd new_pfd;
        new_pfd.fd = fd;
        new_pfd.events = POLLIN;
        new_pfd.revents = 0;
        impl_->fds_.push_back(new_pfd);
    }
}

void Selector::addWrite(int fd) {
    pollfd* pfd = impl_->findPfd(fd);
    if (pfd) {
        pfd->events |= POLLOUT;
    } else {
        pollfd new_pfd;
        new_pfd.fd = fd;
        new_pfd.events = POLLOUT;
        new_pfd.revents = 0;
        impl_->fds_.push_back(new_pfd);
    }
}

void Selector::removeRead(int fd) {
    pollfd* pfd = impl_->findPfd(fd);
    if (pfd) {
        pfd->events &= ~POLLIN;
        if (pfd->events == 0) {
            remove(fd);
        }
    }
}

void Selector::removeWrite(int fd) {
    pollfd* pfd = impl_->findPfd(fd);
    if (pfd) {
        pfd->events &= ~POLLOUT;
        if (pfd->events == 0) {
            remove(fd);
        }
    }
}

void Selector::remove(int fd) {
    for (auto it = impl_->fds_.begin(); it != impl_->fds_.end(); ++it) {
        if (it->fd == fd) {
            impl_->fds_.erase(it);
            return;
        }
    }
}

int Selector::wait(int timeout_ms) {
    if (impl_->fds_.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 10));
        return 0;
    }
    return poll(impl_->fds_.data(), impl_->fds_.size(), timeout_ms);
}

bool Selector::isReadable(int fd) const {
    for (const auto& pfd : impl_->fds_) {
        if (pfd.fd == fd) {
            return (pfd.revents & POLLIN) != 0;
        }
    }
    return false;
}

bool Selector::isWritable(int fd) const {
    for (const auto& pfd : impl_->fds_) {
        if (pfd.fd == fd) {
            return (pfd.revents & POLLOUT) != 0;
        }
    }
    return false;
}

// TcpServer实现
class TcpServer::Impl {
public:
    Socket listen_socket_;
    std::atomic<bool> running_{false};
    NewConnectionCallback callback_;
    std::thread thread_;

    void runLoop() {
        Selector selector;
        selector.addRead(listen_socket_.getFd());

        while (running_) {
            int ret = selector.wait(100);
            if (ret > 0 && selector.isReadable(listen_socket_.getFd())) {
                auto client = listen_socket_.accept();
                if (client && callback_) {
                    callback_(std::move(client));
                }
            }
        }
    }
};

TcpServer::TcpServer() : impl_(std::make_unique<Impl>()) {}
TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(const std::string& ip, uint16_t port) {
    if (!impl_->listen_socket_.bind(ip, port)) {
        return false;
    }
    if (!impl_->listen_socket_.listen()) {
        return false;
    }
    if (!impl_->listen_socket_.setNonBlocking(true)) {
        return false;
    }

    impl_->running_ = true;
    impl_->thread_ = std::thread([this]() {
        impl_->runLoop();
    });

    return true;
}

void TcpServer::stop() {
    impl_->running_ = false;
    if (impl_->thread_.joinable()) {
        impl_->thread_.join();
    }
    impl_->listen_socket_.close();
}

void TcpServer::setNewConnectionCallback(NewConnectionCallback callback) {
    impl_->callback_ = callback;
}

void TcpServer::run() {
    if (!impl_->running_) return;
    impl_->thread_.join();
}

} // namespace rtsp
