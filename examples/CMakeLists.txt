# 示例程序

# Server 示例
add_executable(example_server server_example.cpp)
target_link_libraries(example_server PRIVATE rtsp-sdk)

# Client 示例（阻塞方式）
add_executable(example_client client_example.cpp)
target_link_libraries(example_client PRIVATE rtsp-sdk)

# Client 示例（回调方式）
add_executable(player_callback_example player_callback_example.cpp)
target_link_libraries(player_callback_example PRIVATE rtsp-sdk)

# 手动推流示例
add_executable(example_manual_push manual_push_example.cpp)
target_link_libraries(example_manual_push PRIVATE rtsp-sdk)

# ONVIF 一站式示例（只在启用 ONVIF 时构建）
if(BUILD_ONVIF)
    add_executable(example_onvif_server onvif_server_example.cpp)
    target_link_libraries(example_onvif_server PRIVATE rtsp-sdk)
endif()
