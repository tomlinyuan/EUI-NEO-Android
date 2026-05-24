# CMakeLists.txt for EUI-NEO Android native library
cmake_minimum_required(VERSION 3.22.1)
project(eui-neo-native CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Paths
set(EUI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SHIM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp)

# Include directories (shims first to override original GLFW/glad)
include_directories(
    ${SHIM_ROOT}/glfw_shim
    ${SHIM_ROOT}/glad_gles
    ${EUI_ROOT}
    ${EUI_ROOT}/3rd
)

# Source files
set(SHIM_SOURCES
    ${SHIM_ROOT}/glfw_shim/glfw_shim.cpp
)

set(ANDROID_SOURCES
    ${SHIM_ROOT}/android_main.cpp
    ${SHIM_ROOT}/android_app.cpp
)

set(EUI_CORE_SOURCES
    ${EUI_ROOT}/core/async.cpp
    ${EUI_ROOT}/core/image.cpp
    ${EUI_ROOT}/core/ime_bridge.c
    ${EUI_ROOT}/core/network.cpp
    ${EUI_ROOT}/core/platform.cpp
    ${EUI_ROOT}/core/text.cpp
    ${EUI_ROOT}/core/tray_bridge.c
)

# Build shared library
add_library(eui-neo SHARED
    ${SHIM_SOURCES}
    ${ANDROID_SOURCES}
    ${EUI_CORE_SOURCES}
)

# Compile definitions
target_compile_definitions(eui-neo PRIVATE
    __ANDROID__
    GLFW_INCLUDE_NONE
)

# Force-include GLSL compatibility header
target_compile_options(eui-neo PRIVATE
    -include ${SHIM_ROOT}/eui_android_config.h
)

# Link Android system libraries
find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(glesv3-lib GLESv3)

target_link_libraries(eui-neo
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${glesv3-lib}
)

# Compiler flags
target_compile_options(eui-neo PRIVATE
    -Os
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -Wall
    -Wextra
)
