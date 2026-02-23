#include <rtsp-common/socket.h>
#include <rtsp-common/common.h>

#include <cstring>
#include <vector>
#include <thread>
#include <atomic>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
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
    int fd_ = -1;
    std::string local_ip_;
    uint16_t local_port_ = 0;
    std::string peer_ip_;
    uint16_t peer_port_ = 0;

    Impl() = default;
    explicit Impl(int fd) : fd_(fd) {
        if (fd_ >= 0) {
            updateLocalAddr();
            updatePeerAddr();
        }
    }

    ~Impl() {
        close();
    }

    void close() {
        if (fd_ >= 0) {
#ifdef _WIN32
            ::closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_ = -1;
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
    
    return std::make_unique<Socket>(client_fd);
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
    return true;
}

bool Socket::bindUdp(const std::string& ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;

    impl_->fd_ = fd;

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

ssize_t Socket::recv(uint8_t* buffer, size_t size, int timeout_ms) {
    if (impl_->fd_ < 0) return -1;

    if (timeout_ms >= 0) {
        pollfd pfd;
        pfd.fd = impl_->fd_;
        pfd.events = POLLIN;
        
        int res = poll(&pfd, 1, timeout_ms);
        if (res <= 0) return res;
    }

    return ::recv(impl_->fd_, (char*)buffer, size, 0);
}

void Socket::close() {
    impl_->close();
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
