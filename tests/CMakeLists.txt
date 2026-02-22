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

# 客户端RTP聚合包回归测试
add_executable(rtsp_test_client_rtp_aggregation test_client_rtp_aggregation.cpp)
target_link_libraries(rtsp_test_client_rtp_aggregation PRIVATE rtsp-sdk)
add_test(NAME test_client_rtp_aggregation COMMAND rtsp_test_client_rtp_aggregation)
