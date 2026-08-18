cmake_minimum_required(VERSION 3.20)
project(IObject VERSION 1.0.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)

add_library(IObject STATIC
    src/RuntimeObject.cpp
    src/RuntimeDomain.cpp
    src/RuntimeBridge.cpp
)
add_library(IObject::IObject ALIAS IObject)

target_compile_features(IObject PUBLIC cxx_std_20)
target_include_directories(IObject
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(IObject PRIVATE /W4 /utf-8)
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
    add_subdirectory(tests)
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
