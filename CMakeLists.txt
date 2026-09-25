cmake_minimum_required(VERSION 3.20)
project(NoMansSkyHeadTracking VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        d94c64d32ea37bc4f5ee47d580709f70c6fb6080  # v1.3.4-14, 2026-06-13
)
FetchContent_MakeAvailable(minhook)

set(CAMERAUNLOCK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_BUILD_HOOKS ON CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_BUILD_DISCOVERY ON CACHE BOOL "" FORCE)
add_subdirectory(cameraunlock-core/cpp ${CMAKE_BINARY_DIR}/cameraunlock-core)

add_library(NoMansSkyHeadTracking SHARED
    src/pch.cpp
    src/dllmain.cpp
    src/xinput_proxy.cpp
    src/core/mod.cpp
    src/core/config.cpp
    src/legacy_config/legacy_config.cpp
    src/core/early_diag.cpp
    src/core/game_state.cpp
    src/core/window_centering.cpp
    src/input/hotkey_handler.cpp
    src/camera/camera_hook.cpp
    src/camera/build_resolver.cpp
    src/camera/build_profile.cpp
    src/camera/hw_breakpoint.cpp
    src/camera/read_watch.cpp
    src/camera/frame_phase.cpp
    src/camera/commit_hook.cpp
    src/camera/scene_sample.cpp
    src/camera/weapon_probe.cpp
    src/camera/crosshair_probe.cpp
    src/camera/weapon_decouple.cpp
    src/camera/screen_projection.cpp
    src/camera/reticle.cpp
    src/xinput.def
)

target_precompile_headers(NoMansSkyHeadTracking PRIVATE src/pch.h)

# Ship as XINPUT9_1_0.dll - NMS.exe statically imports XInputGetState/
# XInputSetState from it, and the app directory wins over System32 for
# non-KnownDLLs, so our copy in Binaries is loaded at startup. Export
# forwarding to the genuine System32 DLL is handled at runtime via
# LoadLibrary + GetProcAddress in xinput_proxy.cpp.
set_target_properties(NoMansSkyHeadTracking PROPERTIES
    OUTPUT_NAME "XINPUT9_1_0"
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin/Release"
)

target_link_libraries(NoMansSkyHeadTracking PRIVATE
    cameraunlock
    cameraunlock_hooks
    cameraunlock_discovery
    minhook
    ws2_32
    Psapi
)

target_include_directories(NoMansSkyHeadTracking PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(NoMansSkyHeadTracking PRIVATE /W4 /WX /MP /wd4324 /EHa)
endif()

# Tests. Every suite runs without NMS: the tracking-state verdict walk and the
# camera maths are deliberately free of game, clock and logging.
enable_testing()

# Per-config output directory. With one shared path the two configurations
# write the same .exe, so a Release package followed by `ctest -C Debug` runs
# Release binaries under a Debug label.
foreach(nmsht_test state_gate_test udp_recovery_test camera_math_test
                   aim_projection_test)
    add_executable(${nmsht_test} tests/${nmsht_test}.cpp)
    set_target_properties(${nmsht_test} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    target_include_directories(${nmsht_test} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp/include)
    if(MSVC)
        target_compile_options(${nmsht_test} PRIVATE /W4 /WX /MP /EHsc)
    endif()
    add_test(NAME ${nmsht_test} COMMAND ${nmsht_test})
endforeach()

# The recovery suite drives the real receiver against a real held socket, so it
# needs the core library rather than headers alone.
target_link_libraries(udp_recovery_test PRIVATE cameraunlock ws2_32)
set_tests_properties(udp_recovery_test PROPERTIES TIMEOUT 120)

# The config suite drives the mod's own reader against real ini files on disk,
# so it compiles src/core/config.cpp rather than a copy of its rules.
add_executable(config_test tests/config_test.cpp src/core/config.cpp
    src/legacy_config/legacy_config.cpp)
set_target_properties(config_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
target_include_directories(config_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp/include)
target_link_libraries(config_test PRIVATE cameraunlock ws2_32)
if(MSVC)
    target_compile_options(config_test PRIVATE /W4 /WX /MP /EHsc)
endif()
add_test(NAME config_test COMMAND config_test)

add_executable(build_profile_test tests/build_profile_test.cpp)
set_target_properties(build_profile_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
target_include_directories(build_profile_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp/include)
if(MSVC)
    target_compile_options(build_profile_test PRIVATE /W4 /WX /EHsc)
endif()
target_link_libraries(build_profile_test PRIVATE cameraunlock)
add_test(NAME build_profile_test COMMAND build_profile_test)

add_executable(commit_hook_test tests/commit_hook_test.cpp)
set_target_properties(commit_hook_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
target_include_directories(commit_hook_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(commit_hook_test PRIVATE cameraunlock ws2_32)
if(MSVC)
    target_compile_options(commit_hook_test PRIVATE /W4 /WX /EHsc)
endif()
add_test(NAME commit_hook_test COMMAND commit_hook_test)
set_tests_properties(commit_hook_test PROPERTIES TIMEOUT 30)

# The canonical config conversion's differential test. The oracle is v0.1.0's
# reader and startup code: byte copies of its files in
# tests/config_differential/oracle, built against the core ADS header it
# included at its pin (c480d8a), which core has since removed. It is its own
# executable, so its NMSHT::Config never meets the runtime one.
set(NMS_DIFFERENTIAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential)
add_executable(nms_config_oracle
    ${NMS_DIFFERENTIAL_DIR}/oracle/oracle_main.cpp
    ${NMS_DIFFERENTIAL_DIR}/oracle/config.cpp)
target_include_directories(nms_config_oracle PRIVATE
    ${NMS_DIFFERENTIAL_DIR}/oracle
    ${NMS_DIFFERENTIAL_DIR}/oracle/core-c480d8a
    ${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp/include)
target_link_libraries(nms_config_oracle PRIVATE cameraunlock ws2_32)

add_executable(nms_config_differential
    ${NMS_DIFFERENTIAL_DIR}/differential_test.cpp
    src/legacy_config/legacy_config.cpp)
target_include_directories(nms_config_differential PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NMS_DIFFERENTIAL_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp/include)
target_compile_definitions(nms_config_differential PRIVATE
    NMS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nms_config_differential PRIVATE cameraunlock ws2_32)

foreach(nms_differential_target nms_config_oracle nms_config_differential)
    set_target_properties(${nms_differential_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    if(MSVC)
        target_compile_options(${nms_differential_target} PRIVATE /W4 /WX /MP /EHsc)
    endif()
endforeach()
add_test(NAME nms_config_differential
         COMMAND nms_config_differential $<TARGET_FILE:nms_config_oracle>)

# What the differential test compiles is pinned by hash, line endings aside: the
# oracle's files must stay v0.1.0's (git show v0.1.0:<path>, and core c480d8a's
# ADS header), the core sources both readers compile must stay the ones v0.1.0
# was built with (core c480d8a), and the frozen reader is never edited.
function(nms_require_hash path expected)
    file(READ "${path}" text)
    string(REPLACE "\r" "" text "${text}")
    string(SHA256 actual "${text}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "${path} has changed (SHA-256 of its text ${actual}, pinned "
                            "${expected}). The config differential test compares against "
                            "exactly this file; see CMakeLists.txt.")
    endif()
endfunction()
set(NMS_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp)
foreach(pinned
        "${NMS_DIFFERENTIAL_DIR}/oracle/config.cpp|7ddc3105db8ce454a93d407e029ce65d3fe0f4b2d70d407319a34c0f8db69f8e"
        "${NMS_DIFFERENTIAL_DIR}/oracle/config.h|37d479a87713236016ff4c5f8d5840a1f3383918e53a8ea08df427a88e0bf25c"
        "${NMS_DIFFERENTIAL_DIR}/oracle/debug_log.h|f9d5221e0a745777eb54eaab121653b6eaa4306a020d573d442c9403bd835e23"
        "${NMS_DIFFERENTIAL_DIR}/oracle/pch.h|ac4ce9cd43c495f04a90f1858292e0e996ec7f332af6c5d739d9a57d1cb53ff8"
        "${NMS_DIFFERENTIAL_DIR}/oracle/core/constants.h|ee9ae148981fe8cde5d748caa54a0f54487809e7d51f8ab4b9fd5022d06a6d16"
        "${NMS_DIFFERENTIAL_DIR}/oracle/core-c480d8a/cameraunlock/ads/ads_mode.h|94cd36b585e878e673566f602e9496417cb797970162fcc9c2af1e50e24358de"
        "${NMS_CORE_DIR}/include/cameraunlock/config/ini_reader.h|a7ffb44210ff59672fa97e8e5feaa2cb3e81938fcc0334a384c68bc371b3857a"
        "${NMS_CORE_DIR}/src/config/ini_reader.cpp|e01515c2656aaf533bae4350dc45b702c3e3d4043935743dcc9bd5ea581fbe1c"
        "${NMS_CORE_DIR}/include/cameraunlock/config/value_guards.h|6d3e3512bdfc9ac2a54bee750c5425191bdb4c24f3ae2ea3b9925612e01c78e5"
        "${NMS_CORE_DIR}/src/config/value_guards.cpp|a833ff7f2721f7974ce039a3597349a0974e6695ce3e75e598ce5bf4985a14b1"
        "${NMS_CORE_DIR}/include/cameraunlock/math/finite_utils.h|c59772d698d54ade3374ee1221b74f5563a86d76f0eb34a989ef7efab389c0ad"
        "${NMS_CORE_DIR}/include/cameraunlock/protocol/port_utils.h|91bff564d5e279b66527ec5553afcf4d591812dab0e78f71a48ef412e4db7a44"
        "${NMS_CORE_DIR}/include/cameraunlock/data/position_settings.h|b24dceb8e25475aebc5a468a5c7362a4a4e64204d183d1408525345f32f547f5"
        "${NMS_CORE_DIR}/include/cameraunlock/math/smoothing_utils.h|fc2146f8c585e5f610c7234e302f59de4945679cfa28ff479ca47477ec073f22"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/legacy_config/legacy_config.h|59e739a7b4501e81aa77c87b57e66090bde33867774a2e40d295cd1340312cc1"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/legacy_config/legacy_config.cpp|cafb66338c1dfd085fdd5e7592f0603d9e742bf6f715e57df53ed7dc9afdfc0b")
    string(REPLACE "|" ";" pair "${pinned}")
    list(GET pair 0 pinned_path)
    list(GET pair 1 pinned_hash)
    nms_require_hash("${pinned_path}" "${pinned_hash}")
endforeach()
