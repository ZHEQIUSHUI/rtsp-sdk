# rtsp-cli — RTSP 拉流/推流命令行工具

add_executable(rtsp-cli
    main.cpp
    pull.cpp
    push.cpp
)
target_link_libraries(rtsp-cli PRIVATE rtsp-sdk)
