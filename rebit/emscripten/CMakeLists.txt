cmake_minimum_required(VERSION 3.20)

project(rebit_melonds_dual LANGUAGES C CXX)

option(REBIT_MELONDS_DUAL_COOPERATIVE "Build the single-threaded cooperative WebAssembly runtime" OFF)
option(REBIT_MELONDS_DUAL_BUILD_CLI "Build the native command-line benchmark" ON)
option(REBIT_MELONDS_DUAL_NATIVE_JIT "Enable melonDS JIT execution in native builds" OFF)
option(REBIT_MELONDS_ROLLBACK "Build the isolated experimental rollback runtime" OFF)
option(REBIT_MELONDS_ROLLBACK_PLAYGROUND "Enable local-only dual-screen diagnostics" OFF)
if(REBIT_MELONDS_ROLLBACK_PLAYGROUND AND NOT REBIT_MELONDS_ROLLBACK)
    message(FATAL_ERROR "The playground requires the rollback runtime")
endif()
if(REBIT_MELONDS_ROLLBACK AND (REBIT_MELONDS_DUAL_COOPERATIVE OR REBIT_MELONDS_DUAL_NATIVE_JIT))
    message(FATAL_ERROR "Rollback currently requires the parallel interpreter runtime")
endif()

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
set(ENABLE_JIT ${REBIT_MELONDS_DUAL_NATIVE_JIT} CACHE BOOL "" FORCE)
set(ENABLE_JIT_PROFILING OFF CACHE BOOL "" FORCE)
set(ENABLE_OGLRENDERER OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO_RELEASE OFF CACHE BOOL "" FORCE)

add_subdirectory("${MELONDS_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/melonds" EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

set(REBIT_MELONDS_DUAL_RUNTIME_SOURCES
    src/melonds_dual.cpp
    src/platform.cpp
    "${MELONDS_SOURCE_DIR}/src/net/LocalMP.cpp"
)

if(EMSCRIPTEN)
    add_executable(melonds_dual ${REBIT_MELONDS_DUAL_RUNTIME_SOURCES})
    set(REBIT_MELONDS_DUAL_RUNTIME_TARGET melonds_dual)
else()
    add_library(rebit_melonds_dual_runtime STATIC ${REBIT_MELONDS_DUAL_RUNTIME_SOURCES})
    set(REBIT_MELONDS_DUAL_RUNTIME_TARGET rebit_melonds_dual_runtime)
    target_include_directories(rebit_melonds_dual_runtime PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    if(REBIT_MELONDS_DUAL_BUILD_CLI)
        add_executable(melonds_dual src/cli.cpp)
        target_link_libraries(melonds_dual PRIVATE rebit_melonds_dual_runtime)
    endif()
endif()

target_include_directories(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MELONDS_SOURCE_DIR}/src"
    "${MELONDS_SOURCE_DIR}/src/net"
    "${CMAKE_CURRENT_BINARY_DIR}/melonds/src"
)

target_link_libraries(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE core)
target_compile_definitions(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE REBIT_MELONDS_DUAL_SCHEDULED=1)
target_compile_definitions(core PUBLIC REBIT_MELONDS_NDS_ONLY=1)
if(REBIT_MELONDS_ROLLBACK)
    target_compile_definitions(core PUBLIC REBIT_MELONDS_ROLLBACK=1)
endif()
if(NOT REBIT_MELONDS_DUAL_COOPERATIVE)
    target_link_libraries(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE Threads::Threads)
endif()
set_property(TARGET ${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PROPERTY CXX_STANDARD 20)
if(REBIT_MELONDS_DUAL_COOPERATIVE)
    target_compile_definitions(core PUBLIC REBIT_MELONDS_DUAL_COOPERATIVE=1)
    target_compile_definitions(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE
        REBIT_MELONDS_DUAL_BUILD="melonds-dual-cooperative-1"
        REBIT_MELONDS_DUAL_RUNTIME_ABI="rebit-melonds-dual-cooperative-1"
        REBIT_MELONDS_DUAL_COOPERATIVE=1
    )
elseif(REBIT_MELONDS_ROLLBACK)
    if(REBIT_MELONDS_ROLLBACK_PLAYGROUND)
        set(REBIT_ROLLBACK_BUILD "melonds-dual-rollback-playground-1")
        target_compile_definitions(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE REBIT_MELONDS_ROLLBACK_PLAYGROUND=1)
    else()
        set(REBIT_ROLLBACK_BUILD "melonds-dual-rollback-experimental-2")
    endif()
    target_compile_definitions(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE
        REBIT_MELONDS_DUAL_BUILD="${REBIT_ROLLBACK_BUILD}"
        REBIT_MELONDS_DUAL_RUNTIME_ABI="rebit-melonds-rollback-experimental-v1"
    )
else()
    target_compile_definitions(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE
        REBIT_MELONDS_DUAL_BUILD="melonds-dual-parallel-1"
        REBIT_MELONDS_DUAL_RUNTIME_ABI="rebit-melonds-parallel-v1"
    )
endif()
if(REBIT_MELONDS_DUAL_NATIVE_JIT)
    target_compile_definitions(${REBIT_MELONDS_DUAL_RUNTIME_TARGET} PRIVATE REBIT_MELONDS_DUAL_NATIVE_JIT=1)
endif()

if(REBIT_MELONDS_ROLLBACK AND NOT EMSCRIPTEN)
    enable_testing()
    add_executable(nds_rollback_scheduler_test src/scheduler_test.cpp)
    target_include_directories(nds_rollback_scheduler_test PRIVATE
        $<TARGET_PROPERTY:rebit_melonds_dual_runtime,INCLUDE_DIRECTORIES>)
    target_compile_definitions(nds_rollback_scheduler_test PRIVATE
        $<TARGET_PROPERTY:rebit_melonds_dual_runtime,COMPILE_DEFINITIONS>)
    target_link_libraries(nds_rollback_scheduler_test PRIVATE rebit_melonds_dual_runtime core)
    set_property(TARGET nds_rollback_scheduler_test PROPERTY CXX_STANDARD 20)
    add_test(NAME nds_rollback_scheduler COMMAND nds_rollback_scheduler_test)
    set_tests_properties(nds_rollback_scheduler PROPERTIES TIMEOUT 10)
endif()

if(EMSCRIPTEN)
    if(REBIT_MELONDS_DUAL_COOPERATIVE)
        set(REBIT_MELONDS_DUAL_EXPORT_NAME createMelondsDualCooperativeModule)
    elseif(REBIT_MELONDS_ROLLBACK)
        set(REBIT_MELONDS_DUAL_EXPORT_NAME createMelondsDualRollbackModule)
    else()
        set(REBIT_MELONDS_DUAL_EXPORT_NAME createMelondsDualModule)
    endif()

    target_compile_options(core PUBLIC "-msimd128")
    target_compile_options(teakra PUBLIC "-msimd128")
    target_compile_options(melonds_dual PRIVATE "-msimd128")
    target_compile_options(core PUBLIC "-flto")
    target_compile_options(teakra PUBLIC "-flto")
    target_compile_options(melonds_dual PRIVATE "-flto")

    if(NOT REBIT_MELONDS_DUAL_COOPERATIVE)
        target_compile_options(core PUBLIC "-pthread")
        target_compile_options(teakra PUBLIC "-pthread")
        target_compile_options(melonds_dual PRIVATE "-pthread")
    endif()

    target_link_options(melonds_dual PRIVATE
        "--no-entry"
        "-flto"
        "-msimd128"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=${REBIT_MELONDS_DUAL_EXPORT_NAME}"
        "-sENVIRONMENT=worker,node"
        "-sALLOW_MEMORY_GROWTH=0"
        "-sINITIAL_MEMORY=1073741824"
        "-sSTACK_SIZE=2097152"
        "-sNO_EXIT_RUNTIME=1"
        "-sDISABLE_EXCEPTION_CATCHING=0"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','HEAP16','ccall','cwrap','UTF8ToString']"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_md_api_version','_md_runtime_abi','_md_load','_md_destroy','_md_is_loaded','_md_player_count','_md_set_visible_player','_md_visible_player','_md_set_input','_md_run_frame','_md_frame','_md_framebuffer','_md_width','_md_height','_md_audio_sample_rate','_md_audio_available','_md_audio_read','_md_audio_buffer','_md_save_size','_md_save_data','_md_import_save','_md_export_checkpoint','_md_checkpoint_size','_md_checkpoint_data','_md_clear_checkpoint','_md_import_checkpoint','_md_set_scheduler_jitter','_md_inject_desync_for_test','_md_state_hash','_md_mp_packets_sent','_md_mp_packets_received','_md_mp_commands','_md_mp_replies','_md_last_frame_ms','_md_build_id','_md_last_error']"
    )

    if(NOT REBIT_MELONDS_DUAL_COOPERATIVE)
        set(REBIT_THREAD_POOL_SIZE 3)
        if(REBIT_MELONDS_ROLLBACK_PLAYGROUND)
            # Two console workers plus two live 3D renderers.
            set(REBIT_THREAD_POOL_SIZE 4)
        endif()
        target_link_options(melonds_dual PRIVATE
            "-pthread"
            "-sPTHREAD_POOL_SIZE=${REBIT_THREAD_POOL_SIZE}"
        )
    endif()

    set_target_properties(melonds_dual PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist"
        OUTPUT_NAME "melonds_dual"
        SUFFIX ".js"
    )
    if(REBIT_MELONDS_ROLLBACK)
        set_target_properties(melonds_dual PROPERTIES OUTPUT_NAME "melonds_dual_rollback")
    endif()
endif()
