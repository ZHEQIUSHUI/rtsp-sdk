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

# 创建库（单一库）
if(BUILD_SHARED)
    add_library(rtsp-sdk SHARED ${RTSP_SDK_SOURCES})
else()
    add_library(rtsp-sdk STATIC ${RTSP_SDK_SOURCES})
endif()

# 包含目录
target_include_directories(rtsp-sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

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
