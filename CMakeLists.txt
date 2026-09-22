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
    src/core/ads.cpp
    src/core/config.cpp
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

# Tests. Every suite runs without NMS: the ADS pose pipeline, the
# tracking-state verdict walk and the camera maths are deliberately free of
# game, clock and logging, so an entire aim can be driven frame by frame here.
enable_testing()

# Per-config output directory. With one shared path the two configurations
# write the same .exe, so a Release package followed by `ctest -C Debug` runs
# Release binaries under a Debug label.
foreach(nmsht_test ads_pose_test state_gate_test udp_recovery_test camera_math_test
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
add_executable(config_test tests/config_test.cpp src/core/config.cpp)
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
