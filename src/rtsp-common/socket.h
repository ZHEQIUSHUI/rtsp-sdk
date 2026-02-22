#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <memory>

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
    ssize_t recv(uint8_t* buffer, size_t size, int timeout_ms = -1);
    
    void close();
    bool setNonBlocking(bool non_blocking);
    bool setReuseAddr(bool reuse);
    bool setSendBufferSize(int size);
    bool setRecvBufferSize(int size);
    
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
