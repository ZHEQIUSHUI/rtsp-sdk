cmake_minimum_required(VERSION 3.10)
project(rtsp-sdk VERSION 1.0.0 LANGUAGES CXX)

# C++标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 选项
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_SHARED "Build shared library" OFF)
# ONVIF 子模块：局域网发现 + Profile S 最小 SOAP 服务。
# 关闭时完全不编译 onvif/*，也不依赖 third_party/httplib.h。
option(BUILD_ONVIF "Build ONVIF discovery + SOAP service" ON)
# RTMP Publisher 子模块：把 H.264/H.265 流推到外部 RTMP server（CDN / 直播）。
# 关闭时完全不编译 rtmp/*。
option(BUILD_RTMP "Build RTMP publisher" ON)

# 所有源文件
set(RTSP_SDK_SOURCES
    src/rtsp-common/common.cpp
    src/rtsp-common/socket.cpp
    src/rtsp-common/rtsp_request.cpp
    src/rtsp-common/sdp.cpp
    src/rtsp-common/rtp_packer.cpp
    src/server/rtsp_server.cpp
    src/client/rtsp_client.cpp
    src/publisher/rtsp_publisher.cpp
)

# 所有头文件（用于安装）
set(RTSP_SDK_HEADERS
    include/rtsp-common/common.h
    include/rtsp-server/rtsp_server.h
    include/rtsp-server/rtsp-server.h
    include/rtsp-client/rtsp_client.h
    include/rtsp-client/rtsp-client.h
    include/rtsp-publisher/rtsp_publisher.h
    include/rtsp-publisher/rtsp-publisher.h
)

# ONVIF 模块（可选）
if(BUILD_ONVIF)
    list(APPEND RTSP_SDK_SOURCES
        src/onvif/onvif_daemon.cpp
        src/onvif/soap_endpoint.cpp
        src/onvif/ws_discovery.cpp
        src/onvif/wsse_auth.cpp
    )
    list(APPEND RTSP_SDK_HEADERS
        include/rtsp-onvif/onvif_daemon.h
        include/rtsp-onvif/rtsp-onvif.h
    )
endif()

# RTMP Publisher 模块（可选）
if(BUILD_RTMP)
    list(APPEND RTSP_SDK_SOURCES
        src/rtmp/rtmp_publisher.cpp
        src/rtmp/rtmp_handshake.cpp
        src/rtmp/rtmp_chunk_stream.cpp
        src/rtmp/amf0_codec.cpp
        src/rtmp/flv_tag_encoder.cpp
        src/rtmp/avc_config_record.cpp
        src/rtmp/hevc_config_record.cpp
    )
    list(APPEND RTSP_SDK_HEADERS
        include/rtsp-rtmp/rtmp_publisher.h
        include/rtsp-rtmp/rtsp-rtmp.h
    )
endif()

# 创建库（单一库）
if(BUILD_SHARED)
    add_library(rtsp-sdk SHARED ${RTSP_SDK_SOURCES})
else()
    add_library(rtsp-sdk STATIC ${RTSP_SDK_SOURCES})
endif()

# Make the static archive safe to link into downstream shared libraries.
set_target_properties(rtsp-sdk PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# 包含目录
target_include_directories(rtsp-sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

# ONVIF 专用：httplib 单头文件路径（仅构建期可见，外部消费者不需要）
if(BUILD_ONVIF)
    target_include_directories(rtsp-sdk
        PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
    target_compile_definitions(rtsp-sdk PUBLIC RTSP_SDK_WITH_ONVIF=1)
    # Windows 下 iphlpapi 用来枚举本机 IP
    if(WIN32)
        target_link_libraries(rtsp-sdk PUBLIC iphlpapi)
    endif()
endif()

if(BUILD_RTMP)
    target_compile_definitions(rtsp-sdk PUBLIC RTSP_SDK_WITH_RTMP=1)
endif()

# 编译选项
target_compile_options(rtsp-sdk PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic >
    $<$<CXX_COMPILER_ID:MSVC>: /W4 >
)

# 链接库
if(WIN32)
    target_link_libraries(rtsp-sdk PUBLIC ws2_32)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(rtsp-sdk PUBLIC pthread)
endif()

# 别名（方便使用）
add_library(rtsp::rtsp-sdk ALIAS rtsp-sdk)

# Examples
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install
include(GNUInstallDirs)

install(TARGETS rtsp-sdk
    EXPORT rtsp-sdk-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT rtsp-sdk-targets
    FILE rtsp-sdk-targets.cmake
    NAMESPACE rtsp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtsp-sdk
)

install(FILES ${RTSP_SDK_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtsp-sdk
)

if(BUILD_ONVIF)
    install(DIRECTORY include/rtsp-onvif/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtsp-sdk/rtsp-onvif
            FILES_MATCHING PATTERN "*.h")
endif()

if(BUILD_RTMP)
    install(DIRECTORY include/rtsp-rtmp/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtsp-sdk/rtsp-rtmp
            FILES_MATCHING PATTERN "*.h")
endif()

# CMake config
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rtsp-sdk-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rtsp-sdk-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtsp-sdk
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/rtsp-sdk-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rtsp-sdk-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rtsp-sdk-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtsp-sdk
)
