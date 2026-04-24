# 测试程序

include_directories(${CMAKE_SOURCE_DIR}/src/rtsp-common)

# 基础测试
add_executable(rtsp_test_base test_base.cpp)
target_link_libraries(rtsp_test_base PRIVATE rtsp-sdk)
add_test(NAME test_base COMMAND rtsp_test_base)

# RTSP协议测试
add_executable(rtsp_test_protocol test_protocol.cpp)
target_link_libraries(rtsp_test_protocol PRIVATE rtsp-sdk)
add_test(NAME test_protocol COMMAND rtsp_test_protocol)

# RTP打包测试
add_executable(rtsp_test_rtp test_rtp.cpp)
target_link_libraries(rtsp_test_rtp PRIVATE rtsp-sdk)
add_test(NAME test_rtp COMMAND rtsp_test_rtp)

# 集成测试
add_executable(rtsp_test_integration test_integration.cpp)
target_link_libraries(rtsp_test_integration PRIVATE rtsp-sdk)
add_test(NAME test_integration COMMAND rtsp_test_integration)

# publisher -> builtin server -> client bridge
add_executable(rtsp_test_publish_server_bridge test_publish_server_bridge.cpp)
target_link_libraries(rtsp_test_publish_server_bridge PRIVATE rtsp-sdk)
add_test(NAME test_publish_server_bridge COMMAND rtsp_test_publish_server_bridge)

# 客户端RTP聚合包回归测试
add_executable(rtsp_test_client_rtp_aggregation test_client_rtp_aggregation.cpp)
target_link_libraries(rtsp_test_client_rtp_aggregation PRIVATE rtsp-sdk)
add_test(NAME test_client_rtp_aggregation COMMAND rtsp_test_client_rtp_aggregation)

# 输入解析健壮性（H2/H3）
add_executable(rtsp_test_parse_robustness test_parse_robustness.cpp)
target_link_libraries(rtsp_test_parse_robustness PRIVATE rtsp-sdk)
add_test(NAME test_parse_robustness COMMAND rtsp_test_parse_robustness)

# 恶劣客户端回归（C2 + H3）
add_executable(rtsp_test_hostile_client test_hostile_client.cpp)
target_link_libraries(rtsp_test_hostile_client PRIVATE rtsp-sdk)
add_test(NAME test_hostile_client COMMAND rtsp_test_hostile_client)
set_tests_properties(test_hostile_client PROPERTIES TIMEOUT 30)

# 并发 SETUP + 推帧（C1/C3/C5/H1 在 TSan 下回归）
add_executable(rtsp_test_concurrent_setup test_concurrent_setup.cpp)
target_link_libraries(rtsp_test_concurrent_setup PRIVATE rtsp-sdk)
add_test(NAME test_concurrent_setup COMMAND rtsp_test_concurrent_setup)
set_tests_properties(test_concurrent_setup PROPERTIES TIMEOUT 60)

# DESCRIBE 响应带 Content-Base（M6）
add_executable(rtsp_test_describe_content_base test_describe_content_base.cpp)
target_link_libraries(rtsp_test_describe_content_base PRIVATE rtsp-sdk)
add_test(NAME test_describe_content_base COMMAND rtsp_test_describe_content_base)
set_tests_properties(test_describe_content_base PROPERTIES TIMEOUT 15)

# RTP 与 RTCP SR SSRC 一致性（C4）
add_executable(rtsp_test_rtcp_ssrc test_rtcp_ssrc.cpp)
target_link_libraries(rtsp_test_rtcp_ssrc PRIVATE rtsp-sdk)
add_test(NAME test_rtcp_ssrc COMMAND rtsp_test_rtcp_ssrc)
set_tests_properties(test_rtcp_ssrc PROPERTIES TIMEOUT 30)

# RTMP 相关测试（仅在启用 RTMP 时构建）
if(BUILD_RTMP)
    # 需要访问 src/rtmp/ 下的内部头
    include_directories(${CMAKE_SOURCE_DIR}/src/rtmp)

    # AMF0 encode/decode 单元测试
    add_executable(rtsp_test_amf0 test_amf0.cpp)
    target_link_libraries(rtsp_test_amf0 PRIVATE rtsp-sdk)
    add_test(NAME test_amf0 COMMAND rtsp_test_amf0)
    set_tests_properties(test_amf0 PROPERTIES TIMEOUT 10)

    # FLV tag / AVC config 字节级正确性
    add_executable(rtsp_test_rtmp_flv test_rtmp_flv.cpp)
    target_link_libraries(rtsp_test_rtmp_flv PRIVATE rtsp-sdk)
    add_test(NAME test_rtmp_flv COMMAND rtsp_test_rtmp_flv)
    set_tests_properties(test_rtmp_flv PROPERTIES TIMEOUT 10)

    # 端到端：mock RTMP server + 完整 publish 流程
    add_executable(rtsp_test_rtmp_publisher test_rtmp_publisher.cpp)
    target_link_libraries(rtsp_test_rtmp_publisher PRIVATE rtsp-sdk)
    if(WIN32)
        target_link_libraries(rtsp_test_rtmp_publisher PRIVATE ws2_32)
    endif()
    add_test(NAME test_rtmp_publisher COMMAND rtsp_test_rtmp_publisher)
    set_tests_properties(test_rtmp_publisher PROPERTIES TIMEOUT 30)
endif()

# ONVIF 相关测试（仅在启用 ONVIF 时构建）
if(BUILD_ONVIF)
    # WS-Discovery Probe 往返（可能在不支持多播的 CI 容器中跳过）
    add_executable(rtsp_test_onvif_discovery test_onvif_discovery.cpp)
    target_link_libraries(rtsp_test_onvif_discovery PRIVATE rtsp-sdk)
    if(WIN32)
        target_link_libraries(rtsp_test_onvif_discovery PRIVATE ws2_32)
    endif()
    add_test(NAME test_onvif_discovery COMMAND rtsp_test_onvif_discovery)
    set_tests_properties(test_onvif_discovery PROPERTIES TIMEOUT 15)

    # SOAP 端点 + WS-Security 鉴权 + GetStreamUri 正确性
    add_executable(rtsp_test_onvif_soap test_onvif_soap.cpp)
    target_link_libraries(rtsp_test_onvif_soap PRIVATE rtsp-sdk)
    add_test(NAME test_onvif_soap COMMAND rtsp_test_onvif_soap)
    set_tests_properties(test_onvif_soap PROPERTIES TIMEOUT 15)
endif()
