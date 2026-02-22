/**
 * RTSP协议测试
 */

#include <rtsp-server/rtsp-server.h>
#include "rtsp_request.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace rtsp;

void test_rtsp_request_parsing() {
    std::cout << "Testing RTSP request parsing..." << std::endl;
    
    // 测试OPTIONS请求
    {
        std::string request = 
            "OPTIONS rtsp://example.com/stream RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "User-Agent: Test/1.0\r\n"
            "\r\n";
        
        RtspRequest req;
        assert(req.parse(request));
        assert(req.getMethod() == RtspMethod::Options);
        assert(req.getUri() == "rtsp://example.com/stream");
        assert(req.getPath() == "rtsp://example.com/stream");
        assert(req.getCSeq() == 1);
    }
    
    // 测试DESCRIBE请求
    {
        std::string request = 
            "DESCRIBE rtsp://example.com/live/stream RTSP/1.0\r\n"
            "CSeq: 2\r\n"
            "Accept: application/sdp\r\n"
            "\r\n";
        
        RtspRequest req;
        assert(req.parse(request));
        assert(req.getMethod() == RtspMethod::Describe);
        assert(req.getCSeq() == 2);
    }
    
    // 测试SETUP请求
    {
        std::string request = 
            "SETUP rtsp://example.com/live/stream/streamid=0 RTSP/1.0\r\n"
            "CSeq: 3\r\n"
            "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n"
            "\r\n";
        
        RtspRequest req;
        assert(req.parse(request));
        assert(req.getMethod() == RtspMethod::Setup);
        assert(req.getCSeq() == 3);
        assert(req.getRtpPort() == 5000);
        assert(req.getRtcpPort() == 5001);
    }
    
    // 测试PLAY请求
    {
        std::string request = 
            "PLAY rtsp://example.com/live/stream RTSP/1.0\r\n"
            "CSeq: 4\r\n"
            "Session: 12345678\r\n"
            "Range: npt=0.000-\r\n"
            "\r\n";
        
        RtspRequest req;
        assert(req.parse(request));
        assert(req.getMethod() == RtspMethod::Play);
        assert(req.getSession() == "12345678");
    }
    
    std::cout << "  RTSP request parsing tests passed!" << std::endl;
}

void test_rtsp_response_building() {
    std::cout << "Testing RTSP response building..." << std::endl;
    
    // 测试OK响应
    {
        RtspResponse resp = RtspResponse::createOk(1);
        std::string data = resp.build();
        assert(data.find("RTSP/1.0 200 OK") != std::string::npos);
        assert(data.find("CSeq: 1") != std::string::npos);
    }
    
    // 测试OPTIONS响应
    {
        RtspResponse resp = RtspResponse::createOptions(1);
        std::string data = resp.build();
        assert(data.find("Public:") != std::string::npos);
        assert(data.find("DESCRIBE") != std::string::npos);
    }
    
    // 测试DESCRIBE响应
    {
        std::string sdp = "v=0\r\ns=Test\r\n";
        RtspResponse resp = RtspResponse::createDescribe(2, sdp);
        std::string data = resp.build();
        assert(data.find("Content-Type: application/sdp") != std::string::npos);
        assert(data.find("Content-Length: 13") != std::string::npos);
        assert(data.find(sdp) != std::string::npos);
    }
    
    // 测试SETUP响应
    {
        RtspResponse resp = RtspResponse::createSetup(3, "abc123", 
            "RTP/AVP;unicast;client_port=5000-5001;server_port=6000-6001");
        std::string data = resp.build();
        assert(data.find("Session: abc123") != std::string::npos);
        assert(data.find("Transport:") != std::string::npos);
    }
    
    // 测试错误响应
    {
        RtspResponse resp = RtspResponse::createError(5, 404, "Not Found");
        std::string data = resp.build();
        assert(data.find("RTSP/1.0 404 Not Found") != std::string::npos);
    }
    
    std::cout << "  RTSP response building tests passed!" << std::endl;
}

void test_rtsp_method_parsing() {
    std::cout << "Testing RTSP method parsing..." << std::endl;
    
    assert(RtspRequest::parseMethod("OPTIONS") == RtspMethod::Options);
    assert(RtspRequest::parseMethod("DESCRIBE") == RtspMethod::Describe);
    assert(RtspRequest::parseMethod("SETUP") == RtspMethod::Setup);
    assert(RtspRequest::parseMethod("PLAY") == RtspMethod::Play);
    assert(RtspRequest::parseMethod("PAUSE") == RtspMethod::Pause);
    assert(RtspRequest::parseMethod("TEARDOWN") == RtspMethod::Teardown);
    assert(RtspRequest::parseMethod("ANNOUNCE") == RtspMethod::Announce);
    assert(RtspRequest::parseMethod("GET_PARAMETER") == RtspMethod::GetParameter);
    assert(RtspRequest::parseMethod("SET_PARAMETER") == RtspMethod::SetParameter);
    assert(RtspRequest::parseMethod("UNKNOWN") == RtspMethod::Unknown);
    
    // 大小写不敏感
    assert(RtspRequest::parseMethod("options") == RtspMethod::Options);
    assert(RtspRequest::parseMethod("Play") == RtspMethod::Play);
    
    std::cout << "  RTSP method parsing tests passed!" << std::endl;
}

void test_transport_parsing() {
    std::cout << "Testing Transport header parsing..." << std::endl;
    
    // 单播
    {
        std::string request = 
            "SETUP rtsp://example.com/stream RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n"
            "\r\n";
        
        RtspRequest req;
        req.parse(request);
        assert(req.getRtpPort() == 5000);
        assert(req.getRtcpPort() == 5001);
        assert(!req.isMulticast());
    }
    
    // 多播
    {
        std::string request = 
            "SETUP rtsp://example.com/stream RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "Transport: RTP/AVP;multicast\r\n"
            "\r\n";
        
        RtspRequest req;
        req.parse(request);
        assert(req.isMulticast());
    }
    
    std::cout << "  Transport header parsing tests passed!" << std::endl;
}

int main() {
    std::cout << "=== Running Protocol Tests ===" << std::endl;
    
    try {
        test_rtsp_request_parsing();
        test_rtsp_response_building();
        test_rtsp_method_parsing();
        test_transport_parsing();
        
        std::cout << "\n=== All Protocol Tests Passed! ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
