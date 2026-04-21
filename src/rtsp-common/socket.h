#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <memory>
#include <cstddef>

#ifdef _WIN32
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
#endif

namespace rtsp {

// 跨平台socket封装
class Socket {
public:
    Socket();
    explicit Socket(int fd);
    ~Socket();

    // 禁止拷贝，允许移动
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    // TCP
    bool bind(const std::string& ip, uint16_t port);
    bool listen(int backlog = 128);
    std::unique_ptr<Socket> accept();
    bool connect(const std::string& ip, uint16_t port, int timeout_ms = 5000);
    
    // UDP
    bool bindUdp(const std::string& ip, uint16_t port);
    ssize_t sendTo(const uint8_t* data, size_t size, const std::string& ip, uint16_t port);
    ssize_t recvFrom(uint8_t* buffer, size_t size, std::string& from_ip, uint16_t& from_port);

    // 通用
    ssize_t send(const uint8_t* data, size_t size);
    // 带超时的全量发送：在 timeout_ms 内尽量发完 size 字节，
    // 返回已发送字节数；timeout 命中或 socket 关闭返回 -1。
    // 避免 TCP 对端不读时阻塞主调用链（RTSP interleaved 模式下必需）。
    ssize_t sendAll(const uint8_t* data, size_t size, int timeout_ms);
    ssize_t recv(uint8_t* buffer, size_t size, int timeout_ms = -1);

    void close();
    bool shutdownReadWrite();
    bool setNonBlocking(bool non_blocking);
    bool setReuseAddr(bool reuse);
    bool setSendBufferSize(int size);
    bool setRecvBufferSize(int size);
    // 启用 Nagle 禁用（适用于 TCP）。非 TCP socket 忽略。
    bool setTcpNoDelay(bool enable);
    // 设置内核级发送超时（SO_SNDTIMEO）。对于阻塞 send 保护底线。
    bool setSendTimeout(int timeout_ms);
    // poll socket 可读；timeout_ms=0 立即返回；>0 最多等这么久。
    // 返回值：1 可读，0 超时，-1 错误或已关闭。用于替代非阻塞 recvFrom 的 1ms 忙等。
    int waitReadable(int timeout_ms) const;
    
    bool isValid() const;
    int getFd() const;
    std::string getLocalIp() const;
    uint16_t getLocalPort() const;
    std::string getPeerIp() const;
    uint16_t getPeerPort() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 选择器（用于多路复用）
class Selector {
public:
    Selector();
    ~Selector();

    void addRead(int fd);
    void addWrite(int fd);
    void removeRead(int fd);
    void removeWrite(int fd);
    void remove(int fd);
    
    // 等待I/O事件，返回就绪的fd数量，timeout_ms=-1表示无限等待
    int wait(int timeout_ms = -1);
    
    bool isReadable(int fd) const;
    bool isWritable(int fd) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 工具：从 TCP socket 读取一条完整的 RTSP 消息（status/request line + headers + optional body
// by Content-Length），忽略之后可能 pipeline 过来的额外字节。
// 超时或对端关闭返回 false。公共实现供 client/publisher 复用，避免单次 recv 的脆弱路径。
bool recvRtspMessage(Socket& socket, std::string* out, int timeout_ms);

// TCP服务器
class TcpServer {
public:
    using NewConnectionCallback = std::function<void(std::unique_ptr<Socket>)>;

    TcpServer();
    ~TcpServer();

    bool start(const std::string& ip, uint16_t port);
    void stop();
    void setNewConnectionCallback(NewConnectionCallback callback);
    void run();  // 阻塞运行事件循环

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rtsp
