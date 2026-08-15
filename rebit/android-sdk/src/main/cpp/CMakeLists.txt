cmake_minimum_required(VERSION 3.22.1)

project(rebit_melonds_android LANGUAGES C CXX)

get_filename_component(MELONDS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

set(REBIT_MELONDS_DUAL_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(REBIT_MELONDS_DUAL_COOPERATIVE OFF CACHE BOOL "" FORCE)
set(REBIT_MELONDS_DUAL_NATIVE_JIT OFF CACHE BOOL "" FORCE)
set(MELONDS_SOURCE_DIR "${MELONDS_ROOT}" CACHE PATH "" FORCE)

add_subdirectory(
    "${MELONDS_ROOT}/rebit/emscripten"
    "${CMAKE_CURRENT_BINARY_DIR}/rebit-melonds-runtime"
)

add_library(rebit_melonds_android SHARED rebit_melonds_android.cpp)
set_property(TARGET rebit_melonds_android PROPERTY CXX_STANDARD 20)

target_link_libraries(rebit_melonds_android PRIVATE
    rebit_melonds_dual_runtime
    android
    aaudio
    EGL
    GLESv2
    log
)
