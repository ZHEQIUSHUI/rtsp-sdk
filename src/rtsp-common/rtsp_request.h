#pragma once

#include <string>
#include <map>
#include <sstream>

namespace rtsp {

// RTSP方法
enum class RtspMethod {
    Unknown,
    Options,
    Describe,
    Setup,
    Play,
    Pause,
    Teardown,
    Announce,
    Record,
    GetParameter,
    SetParameter
};

// RTSP请求
class RtspRequest {
public:
    RtspRequest();
    
    // 解析RTSP请求
    bool parse(const std::string& data);
    bool parse(const char* data, size_t len);
    
    // 获取请求信息
    RtspMethod getMethod() const { return method_; }
    const std::string& getUri() const { return uri_; }
    const std::string& getPath() const { return path_; }
    int getCSeq() const;
    const std::string& getHeader(const std::string& name) const;
    const std::string& getBody() const { return body_; }
    
    // 特定头部获取
    std::string getTransport() const;
    std::string getSession() const;
    int getRtpPort() const;
    int getRtcpPort() const;
    bool isMulticast() const;
    
    // 构建请求字符串
    std::string build() const;
    
    // 设置请求信息（用于构建请求）
    void setMethod(RtspMethod method);
    void setUri(const std::string& uri);
    void setCSeq(int cseq);
    void setHeader(const std::string& name, const std::string& value);
    void setBody(const std::string& body);
    
    // 工具函数
    static RtspMethod parseMethod(const std::string& method_str);
    static std::string methodToString(RtspMethod method);

private:
    RtspMethod method_;
    std::string uri_;
    std::string path_;
    std::string version_;
    std::map<std::string, std::string> headers_;
    std::string body_;
    mutable std::string empty_string_;
};

// RTSP响应
class RtspResponse {
public:
    RtspResponse();
    explicit RtspResponse(int cseq);
    
    // 设置响应信息
    void setStatus(int code, const std::string& reason);
    void setCSeq(int cseq);
    void setHeader(const std::string& name, const std::string& value);
    void setBody(const std::string& body);
    void setSession(const std::string& session_id);
    void setTransport(const std::string& transport);
    void setContentType(const std::string& type);
    
    // 解析响应
    bool parse(const std::string& data);
    
    // 构建响应字符串
    std::string build() const;

    // 常见响应快捷方式
    static RtspResponse createOk(int cseq);
    static RtspResponse createError(int cseq, int code, const std::string& reason);
    static RtspResponse createOptions(int cseq);
    static RtspResponse createDescribe(int cseq, const std::string& sdp);
    static RtspResponse createSetup(int cseq, const std::string& session_id, 
                                     const std::string& transport);
    static RtspResponse createPlay(int cseq, const std::string& session_id);
    static RtspResponse createTeardown(int cseq);

private:
    int cseq_;
    int status_code_;
    std::string status_reason_;
    std::map<std::string, std::string> headers_;
    std::string body_;
};

} // namespace rtsp
