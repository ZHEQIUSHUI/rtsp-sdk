#pragma once

// ONVIF SOAP 响应 XML 模板工具。用字符串拼装，不引 XML 库。
// 参考：ONVIF Device Service WSDL + Media Service WSDL。

#include <rtsp-onvif/onvif_daemon.h>
#include "soap_endpoint.h"

#include <sstream>
#include <string>
#include <vector>

namespace rtsp {
namespace soap {

// 统一的 SOAP envelope 头部与尾部
inline std::string envelopeOpen() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<soap:Envelope "
             "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
             "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
             "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
             "xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
           "<soap:Body>";
}

inline std::string envelopeClose() {
    return "</soap:Body></soap:Envelope>";
}

inline std::string faultResponse(const std::string& reason) {
    std::ostringstream oss;
    oss << envelopeOpen()
        << "<soap:Fault>"
        <<   "<soap:Code><soap:Value>soap:Sender</soap:Value></soap:Code>"
        <<   "<soap:Reason><soap:Text xml:lang=\"en\">" << reason << "</soap:Text></soap:Reason>"
        << "</soap:Fault>"
        << envelopeClose();
    return oss.str();
}

// Device: GetDeviceInformation
inline std::string getDeviceInformationResponse(const OnvifDeviceInfo& info) {
    std::ostringstream oss;
    oss << envelopeOpen()
        << "<tds:GetDeviceInformationResponse>"
        <<   "<tds:Manufacturer>" << info.manufacturer << "</tds:Manufacturer>"
        <<   "<tds:Model>"        << info.model        << "</tds:Model>"
        <<   "<tds:FirmwareVersion>" << info.firmware  << "</tds:FirmwareVersion>"
        <<   "<tds:SerialNumber>" << info.serial       << "</tds:SerialNumber>"
        <<   "<tds:HardwareId>"   << info.hardware_id  << "</tds:HardwareId>"
        << "</tds:GetDeviceInformationResponse>"
        << envelopeClose();
    return oss.str();
}

// Device: GetSystemDateAndTime
inline std::string getSystemDateAndTimeResponse() {
    std::time_t now_t = std::time(nullptr);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now_t);
#else
    gmtime_r(&now_t, &tm_utc);
#endif
    std::ostringstream oss;
    oss << envelopeOpen()
        << "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
        <<   "<tt:DateTimeType>Manual</tt:DateTimeType>"
        <<   "<tt:DaylightSavings>false</tt:DaylightSavings>"
        <<   "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone>"
        <<   "<tt:UTCDateTime>"
        <<     "<tt:Time>"
        <<       "<tt:Hour>"   << tm_utc.tm_hour << "</tt:Hour>"
        <<       "<tt:Minute>" << tm_utc.tm_min  << "</tt:Minute>"
        <<       "<tt:Second>" << tm_utc.tm_sec  << "</tt:Second>"
        <<     "</tt:Time>"
        <<     "<tt:Date>"
        <<       "<tt:Year>"  << (tm_utc.tm_year + 1900) << "</tt:Year>"
        <<       "<tt:Month>" << (tm_utc.tm_mon + 1)     << "</tt:Month>"
        <<       "<tt:Day>"   << tm_utc.tm_mday          << "</tt:Day>"
        <<     "</tt:Date>"
        <<   "</tt:UTCDateTime>"
        << "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>"
        << envelopeClose();
    return oss.str();
}

// Device: GetCapabilities  —— 告诉客户端 "Media 服务在这个 URL"
inline std::string getCapabilitiesResponse(const std::string& media_xaddr,
                                           const std::string& device_xaddr) {
    std::ostringstream oss;
    oss << envelopeOpen()
        << "<tds:GetCapabilitiesResponse><tds:Capabilities>"
        <<   "<tt:Device>"
        <<     "<tt:XAddr>" << device_xaddr << "</tt:XAddr>"
        <<     "<tt:Network><tt:IPFilter>false</tt:IPFilter>"
        <<       "<tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
        <<       "<tt:IPVersion6>false</tt:IPVersion6>"
        <<       "<tt:DynDNS>false</tt:DynDNS></tt:Network>"
        <<     "<tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve>"
        <<       "<tt:DiscoveryBye>false</tt:DiscoveryBye>"
        <<       "<tt:RemoteDiscovery>false</tt:RemoteDiscovery>"
        <<       "<tt:SystemBackup>false</tt:SystemBackup>"
        <<       "<tt:SystemLogging>false</tt:SystemLogging>"
        <<       "<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade></tt:System>"
        <<     "<tt:Security><tt:TLS1.1>false</tt:TLS1.1>"
        <<       "<tt:TLS1.2>false</tt:TLS1.2>"
        <<       "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration>"
        <<       "<tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
        <<       "<tt:X.509Token>false</tt:X.509Token>"
        <<       "<tt:SAMLToken>false</tt:SAMLToken>"
        <<       "<tt:KerberosToken>false</tt:KerberosToken>"
        <<       "<tt:RELToken>false</tt:RELToken></tt:Security>"
        <<   "</tt:Device>"
        <<   "<tt:Media>"
        <<     "<tt:XAddr>" << media_xaddr << "</tt:XAddr>"
        <<     "<tt:StreamingCapabilities>"
        <<       "<tt:RTPMulticast>false</tt:RTPMulticast>"
        <<       "<tt:RTP_TCP>true</tt:RTP_TCP>"
        <<       "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
        <<     "</tt:StreamingCapabilities>"
        <<   "</tt:Media>"
        << "</tds:Capabilities></tds:GetCapabilitiesResponse>"
        << envelopeClose();
    return oss.str();
}

// Device: GetServices
inline std::string getServicesResponse(const std::string& media_xaddr,
                                       const std::string& device_xaddr) {
    std::ostringstream oss;
    oss << envelopeOpen()
        << "<tds:GetServicesResponse>"
        <<   "<tds:Service>"
        <<     "<tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
        <<     "<tds:XAddr>" << device_xaddr << "</tds:XAddr>"
        <<     "<tds:Version><tt:Major>2</tt:Major><tt:Minor>42</tt:Minor></tds:Version>"
        <<   "</tds:Service>"
        <<   "<tds:Service>"
        <<     "<tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
        <<     "<tds:XAddr>" << media_xaddr << "</tds:XAddr>"
        <<     "<tds:Version><tt:Major>2</tt:Major><tt:Minor>42</tt:Minor></tds:Version>"
        <<   "</tds:Service>"
        << "</tds:GetServicesResponse>"
        << envelopeClose();
    return oss.str();
}

// Media: VideoSource 片段（每个 profile 一个）
inline std::string videoSourceFragment(const SoapEndpoint::MediaProfile& p) {
    std::ostringstream oss;
    oss << "<tt:VideoSourceConfiguration token=\"VideoSourceToken_" << p.token << "\">"
        <<   "<tt:Name>VideoSource_" << p.name << "</tt:Name>"
        <<   "<tt:UseCount>1</tt:UseCount>"
        <<   "<tt:SourceToken>VideoSource_" << p.token << "</tt:SourceToken>"
        <<   "<tt:Bounds x=\"0\" y=\"0\" width=\"" << p.width << "\" height=\"" << p.height << "\"/>"
        << "</tt:VideoSourceConfiguration>";
    return oss.str();
}

inline std::string videoEncoderFragment(const SoapEndpoint::MediaProfile& p) {
    const std::string encoding = (p.codec == CodecType::H265) ? "H265" : "H264";
    std::ostringstream oss;
    oss << "<tt:VideoEncoderConfiguration token=\"VideoEncoderToken_" << p.token << "\">"
        <<   "<tt:Name>VideoEncoder_" << p.name << "</tt:Name>"
        <<   "<tt:UseCount>1</tt:UseCount>"
        <<   "<tt:Encoding>" << encoding << "</tt:Encoding>"
        <<   "<tt:Resolution>"
        <<     "<tt:Width>"  << p.width  << "</tt:Width>"
        <<     "<tt:Height>" << p.height << "</tt:Height>"
        <<   "</tt:Resolution>"
        <<   "<tt:Quality>5</tt:Quality>"
        <<   "<tt:RateControl>"
        <<     "<tt:FrameRateLimit>" << p.fps << "</tt:FrameRateLimit>"
        <<     "<tt:EncodingInterval>1</tt:EncodingInterval>"
        <<     "<tt:BitrateLimit>4096</tt:BitrateLimit>"
        <<   "</tt:RateControl>"
        <<   "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
        << "</tt:VideoEncoderConfiguration>";
    return oss.str();
}

inline std::string profileFragment(const SoapEndpoint::MediaProfile& p) {
    std::ostringstream oss;
    oss << "<trt:Profiles fixed=\"true\" token=\"" << p.token << "\">"
        <<   "<tt:Name>" << p.name << "</tt:Name>"
        <<   videoSourceFragment(p)
        <<   videoEncoderFragment(p)
        << "</trt:Profiles>";
    return oss.str();
}

// Media: GetProfiles
inline std::string getProfilesResponse(const std::vector<SoapEndpoint::MediaProfile>& profiles) {
    std::ostringstream oss;
    oss << envelopeOpen()
        << "<trt:GetProfilesResponse>";
    for (const auto& p : profiles) oss << profileFragment(p);
    oss << "</trt:GetProfilesResponse>"
        << envelopeClose();
    return oss.str();
}

// Media: GetVideoSources
inline std::string getVideoSourcesResponse(const std::vector<SoapEndpoint::MediaProfile>& profiles) {
    std::ostringstream oss;
    oss << envelopeOpen()
        << "<trt:GetVideoSourcesResponse>";
    for (const auto& p : profiles) {
        oss << "<trt:VideoSources token=\"VideoSource_" << p.token << "\">"
            <<   "<tt:Framerate>" << p.fps << "</tt:Framerate>"
            <<   "<tt:Resolution>"
            <<     "<tt:Width>" << p.width << "</tt:Width>"
            <<     "<tt:Height>" << p.height << "</tt:Height>"
            <<   "</tt:Resolution>"
            << "</trt:VideoSources>";
    }
    oss << "</trt:GetVideoSourcesResponse>"
        << envelopeClose();
    return oss.str();
}

// Media: GetStreamUri — 核心，返回 rtsp://host:port/path
inline std::string getStreamUriResponse(const std::string& rtsp_host, uint16_t rtsp_port,
                                        const std::string& rtsp_path) {
    std::ostringstream uri;
    uri << "rtsp://" << rtsp_host << ":" << rtsp_port;
    if (!rtsp_path.empty() && rtsp_path.front() != '/') uri << "/";
    uri << rtsp_path;

    std::ostringstream oss;
    oss << envelopeOpen()
        << "<trt:GetStreamUriResponse><trt:MediaUri>"
        <<   "<tt:Uri>" << uri.str() << "</tt:Uri>"
        <<   "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
        <<   "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
        <<   "<tt:Timeout>PT60S</tt:Timeout>"
        << "</trt:MediaUri></trt:GetStreamUriResponse>"
        << envelopeClose();
    return oss.str();
}

// Media: GetSnapshotUri  —— 合规"不支持"：返回空 URI，客户端自行降级
inline std::string getSnapshotUriResponse() {
    std::ostringstream oss;
    oss << envelopeOpen()
        << "<trt:GetSnapshotUriResponse><trt:MediaUri>"
        <<   "<tt:Uri></tt:Uri>"
        <<   "<tt:InvalidAfterConnect>true</tt:InvalidAfterConnect>"
        <<   "<tt:InvalidAfterReboot>true</tt:InvalidAfterReboot>"
        <<   "<tt:Timeout>PT0S</tt:Timeout>"
        << "</trt:MediaUri></trt:GetSnapshotUriResponse>"
        << envelopeClose();
    return oss.str();
}

}  // namespace soap
}  // namespace rtsp
