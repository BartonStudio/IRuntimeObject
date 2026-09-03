cmake_minimum_required(VERSION 3.20)
project(IObject VERSION 1.0.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)

add_library(IObject STATIC
    src/RuntimeObject.cpp
    src/RuntimeDomain.cpp
    src/RuntimeBridge.cpp
    src/RuntimeBridgeProtocol.cpp
    src/Executor.cpp
    src/Logger.cpp
    src/WebSocketServer.cpp
    third_party/msgpack11/msgpack11.cpp
)
add_library(IObject::IObject ALIAS IObject)

target_compile_features(IObject PUBLIC cxx_std_20)
target_include_directories(IObject
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack11
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/websocketpp
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include
)

# 内置 WebSocket 服务端（websocketpp + standalone Asio，均为仅头文件依赖）
# WEBSOCKETPP_STANDALONE：websocketpp 通用组件改用 std；ASIO_STANDALONE：asio 不走 Boost。
target_compile_definitions(IObject PRIVATE ASIO_STANDALONE=1 WEBSOCKETPP_STANDALONE=1)
if(WIN32)
    # Asio 需要的 Winsock 版本宏与系统库
    target_compile_definitions(IObject PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(IObject PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(IObject PRIVATE /W4 /utf-8)
    # 如实上报 __cplusplus 宏，websocketpp 据此启用 C++11 模式（不再依赖 Boost）
    target_compile_options(IObject PRIVATE /Zc:__cplusplus)
    target_compile_options(IObject INTERFACE /utf-8)
else()
    target_compile_options(IObject PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(BUILD_EXAMPLES "Build example programs" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

option(BUILD_TESTS "Build automated tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(src/tests)
endif()

install(TARGETS IObject
    EXPORT IObjectTargets
    ARCHIVE DESTINATION lib
    INCLUDES DESTINATION include
)
install(FILES
    include/iobject/IRuntimeObject.hpp
    include/iobject/IRuntimeObjectPointer.hpp
    include/iobject/Runtime.hpp
    include/iobject/RuntimeDomain.hpp
    include/iobject/RuntimeBridge.hpp
    include/iobject/RuntimeBridgeProtocol.hpp
    include/iobject/Executor.hpp
    include/iobject/Logger.hpp
    include/iobject/WebSocketServer.hpp
    DESTINATION include/iobject
)
install(EXPORT IObjectTargets
    FILE IObjectTargets.cmake
    NAMESPACE IObject::
    DESTINATION lib/cmake/IObject
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/IObjectConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/IObjectConfig.cmake
    INSTALL_DESTINATION lib/cmake/IObject
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/IObjectConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/IObjectConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/IObjectConfigVersion.cmake
    DESTINATION lib/cmake/IObject
)
