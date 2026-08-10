cmake_minimum_required(VERSION 3.20)

project(rebit_melonds_dual LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MELONDS_SOURCE_DIR)
    set(MELONDS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
endif()

if(NOT EXISTS "${MELONDS_SOURCE_DIR}/src/NDS.cpp")
    message(FATAL_ERROR "MELONDS_SOURCE_DIR does not point at a standalone melonDS checkout: ${MELONDS_SOURCE_DIR}")
endif()

set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
set(ENABLE_GDBSTUB OFF CACHE BOOL "" FORCE)
set(ENABLE_JIT OFF CACHE BOOL "" FORCE)
set(ENABLE_JIT_PROFILING OFF CACHE BOOL "" FORCE)
set(ENABLE_OGLRENDERER OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO_RELEASE OFF CACHE BOOL "" FORCE)

add_subdirectory("${MELONDS_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/melonds" EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

add_executable(melonds_dual
    src/melonds_dual.cpp
    src/platform.cpp
    "${MELONDS_SOURCE_DIR}/src/net/LocalMP.cpp"
)

target_include_directories(melonds_dual PRIVATE
    "${MELONDS_SOURCE_DIR}/src"
    "${MELONDS_SOURCE_DIR}/src/net"
    "${CMAKE_CURRENT_BINARY_DIR}/melonds/src"
)

target_link_libraries(melonds_dual PRIVATE core Threads::Threads)
target_compile_definitions(melonds_dual PRIVATE REBIT_MELONDS_DUAL_BUILD="melonds-dual-2")

if(EMSCRIPTEN)
    target_compile_options(core PUBLIC "-pthread")
    target_compile_options(teakra PUBLIC "-pthread")
    target_compile_options(melonds_dual PRIVATE "-pthread")

    target_link_options(melonds_dual PRIVATE
        "--no-entry"
        "-pthread"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createMelondsDualModule"
        "-sENVIRONMENT=worker,node"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456"
        "-sMAXIMUM_MEMORY=1073741824"
        "-sPTHREAD_POOL_SIZE=4"
        "-sSTACK_SIZE=2097152"
        "-sNO_EXIT_RUNTIME=1"
        "-sDISABLE_EXCEPTION_CATCHING=0"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','HEAP16','ccall','cwrap','UTF8ToString']"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_md_load','_md_destroy','_md_is_loaded','_md_player_count','_md_set_visible_player','_md_visible_player','_md_set_input','_md_run_frame','_md_frame','_md_framebuffer','_md_width','_md_height','_md_audio_sample_rate','_md_audio_available','_md_audio_read','_md_audio_buffer','_md_save_size','_md_save_data','_md_import_save','_md_state_hash','_md_mp_packets_sent','_md_mp_packets_received','_md_mp_commands','_md_mp_replies','_md_last_frame_ms','_md_build_id','_md_last_error']"
    )

    set_target_properties(melonds_dual PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist"
        OUTPUT_NAME "melonds_dual"
        SUFFIX ".js"
    )
endif()
