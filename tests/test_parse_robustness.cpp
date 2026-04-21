// 输入解析健壮性：畸形输入不得抛异常 / 崩溃（H2/H3 回归）
#include <rtsp-common/common.h>
#include <rtsp-common/rtsp_request.h>
#include <rtsp-common/sdp.h>
#include <cassert>
#include <iostream>
#include <string>

using namespace rtsp;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

static void test_parseIntSafe() {
    int32_t v = -1;
    check(parseInt32Safe("42", v) && v == 42, "parseInt32Safe basic");
    check(parseInt32Safe("   7  ", v) && v == 7, "parseInt32Safe whitespace trimmed");
    check(parseInt32Safe("-123", v) && v == -123, "parseInt32Safe negative");
    check(!parseInt32Safe("", v), "parseInt32Safe empty rejects by default");
    check(parseInt32Safe("", v, /*allow_empty=*/true), "parseInt32Safe empty allowed opt-in");
    check(!parseInt32Safe("abc", v), "parseInt32Safe non-numeric rejects");
    check(!parseInt32Safe("42abc", v), "parseInt32Safe trailing garbage rejects");
    check(!parseInt32Safe("99999999999999", v), "parseInt32Safe overflow rejects");

    uint32_t u = 999;
    check(parseUint32Safe("65535", u) && u == 65535, "parseUint32Safe port");
    check(!parseUint32Safe("-1", u), "parseUint32Safe negative rejects");
    check(!parseUint32Safe("4294967296", u), "parseUint32Safe uint32 overflow rejects");
}

static void test_rtsprequest_malformed_no_throw() {
    // 各类畸形：非数字 CSeq、巨大 CSeq、非数字 client_port
    const char* kMalformed[] = {
        // 非数字 CSeq
        "OPTIONS rtsp://h/a RTSP/1.0\r\nCSeq: abc\r\n\r\n",
        // 非常大 CSeq 会让老代码 std::stoi 抛 out_of_range
        "OPTIONS rtsp://h/a RTSP/1.0\r\nCSeq: 99999999999999\r\n\r\n",
        // Transport 里畸形端口
        "SETUP rtsp://h/a RTSP/1.0\r\nCSeq: 1\r\nTransport: RTP/AVP;client_port=notanumber-badtoo\r\n\r\n",
        // 截断 header
        "DESCRIBE rtsp://h/a RTSP/1.0\r\nCSeq: 2\r\n",
        // 空请求
        "\r\n\r\n",
    };
    for (const char* s : kMalformed) {
        RtspRequest req;
        // 主要诉求：不抛异常。返回值如何不重要。
        bool parsed = req.parse(s);
        (void)parsed;
        (void)req.getCSeq();
        (void)req.getRtpPort();
        (void)req.getRtcpPort();
        (void)req.getTransport();
        (void)req.getSession();
    }
}

static void test_rtsprequest_session_trim() {
    RtspRequest req;
    req.parse("PLAY rtsp://h/a RTSP/1.0\r\nCSeq: 3\r\nSession:   ABC123; timeout=60  \r\n\r\n");
    std::string sid = req.getSession();
    check(sid == "ABC123", "getSession strips timeout parameter and whitespace");
}

static void test_rtsprequest_response_code_safe() {
    RtspResponse r;
    // 畸形状态码不应抛
    r.parse("RTSP/1.0 notacode OK\r\nCSeq: 1\r\n\r\n");
    // 不检查具体值（依旧期望不崩）
}

static void test_sdp_malformed_no_throw() {
    // 各类畸形 SDP：非数字 payload、超大时钟频率、畸形 framesize/cliprect/framerate
    const char* kMalformed[] = {
        "v=0\r\nm=video 0 RTP/AVP abc\r\na=rtpmap:abc H264/xyz\r\na=control:stream\r\n",
        "v=0\r\nm=video 0 RTP/AVP 96\r\na=framesize:96 xxxxx-zzzz\r\n",
        "v=0\r\nm=video 0 RTP/AVP 96\r\na=framerate:notadouble\r\n",
        "v=0\r\nm=video 0 RTP/AVP 999999999999\r\n",  // 整数溢出
        "",
        "garbage",
    };
    for (const char* s : kMalformed) {
        SdpParser p;
        bool ok = p.parse(s);
        (void)ok;
        auto v = p.getVideoInfo();
        (void)v;
    }
}

int main() {
    test_parseIntSafe();
    test_rtsprequest_malformed_no_throw();
    test_rtsprequest_session_trim();
    test_rtsprequest_response_code_safe();
    test_sdp_malformed_no_throw();
    std::cout << "parse robustness tests passed\n";
    return 0;
}
