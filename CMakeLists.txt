cmake_minimum_required(VERSION 3.20)
project(WFASLite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(MSVC)
    # Make Release optimization explicit: some existing Ninja caches had empty
    # CMAKE_CXX_FLAGS_RELEASE, which silently produced an unoptimized binary.
    add_compile_options($<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/Ob2>)
    add_compile_definitions($<$<CONFIG:Release>:NDEBUG>)
endif()

# Protocol, jitter buffer, discovery and reconnect policy.  No WASAPI here so
# the pure logic can be unit tested without an audio device.
add_library(wfas_core
    src/protocol.cpp
    src/jitter_buffer.cpp
    src/discovery.cpp
    src/reconnect.cpp
)
target_include_directories(wfas_core PUBLIC include)
target_compile_definitions(wfas_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wfas_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)
target_link_libraries(wfas_core PUBLIC ws2_32 iphlpapi)

# Session loop, audio sinks and the WASAPI renderer.  Shared by the application
# and by the reconnect integration tests (which use NullSink).
add_library(wfas_client
    src/client_session.cpp
    src/audio_output.cpp
    src/wasapi_renderer.cpp
)
target_link_libraries(wfas_client PUBLIC wfas_core ole32 avrt)
target_compile_definitions(wfas_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(wfas_client PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-lite src/main.cpp)
target_link_libraries(wfas-lite PRIVATE wfas_client)
target_compile_definitions(wfas-lite PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(wfas-lite PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-protocol-tests tests/protocol_tests.cpp)
target_link_libraries(wfas-protocol-tests PRIVATE wfas_core)
target_compile_options(wfas-protocol-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-fake-sender tests/fake_sender.cpp)
target_link_libraries(wfas-fake-sender PRIVATE wfas_core ws2_32)
target_compile_definitions(wfas-fake-sender PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wfas-fake-sender PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-pattern-sender tests/pattern_sender.cpp)
target_link_libraries(wfas-pattern-sender PRIVATE wfas_core ws2_32)
target_compile_definitions(wfas-pattern-sender PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wfas-pattern-sender PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

# Diagnostic tool: a unicast server that also emits discovery beacons.
add_executable(wfas-beacon-sender tests/beacon_sender.cpp)
target_link_libraries(wfas-beacon-sender PRIVATE wfas_core ws2_32 winmm)
target_compile_definitions(wfas-beacon-sender PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wfas-beacon-sender PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-renderer-lifecycle-tests
    tests/renderer_lifecycle_tests.cpp
    src/wasapi_renderer.cpp
)
target_link_libraries(wfas-renderer-lifecycle-tests PRIVATE wfas_core ole32)
target_compile_definitions(wfas-renderer-lifecycle-tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WFAS_LITE_TESTING)
target_compile_options(wfas-renderer-lifecycle-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-audio-pipeline-tests tests/audio_pipeline_tests.cpp)
target_link_libraries(wfas-audio-pipeline-tests PRIVATE wfas_core)
target_compile_options(wfas-audio-pipeline-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-audio-safety-tests tests/audio_safety_tests.cpp)
target_link_libraries(wfas-audio-safety-tests PRIVATE wfas_core)
target_compile_options(wfas-audio-safety-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-network-diagnostics-tests tests/network_diagnostics_tests.cpp)
target_link_libraries(wfas-network-diagnostics-tests PRIVATE wfas_core)
target_compile_options(wfas-network-diagnostics-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-discovery-tests tests/discovery_tests.cpp)
target_link_libraries(wfas-discovery-tests PRIVATE wfas_core)
target_compile_options(wfas-discovery-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

add_executable(wfas-session-tests tests/session_tests.cpp)
target_link_libraries(wfas-session-tests PRIVATE wfas_client)
target_compile_options(wfas-session-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

enable_testing()
add_test(NAME protocol COMMAND wfas-protocol-tests)
add_test(NAME renderer_lifecycle COMMAND wfas-renderer-lifecycle-tests)
add_test(NAME audio_pipeline COMMAND wfas-audio-pipeline-tests)
add_test(NAME audio_safety COMMAND wfas-audio-safety-tests)
add_test(NAME network_diagnostics COMMAND wfas-network-diagnostics-tests)
add_test(NAME discovery COMMAND wfas-discovery-tests)
add_test(NAME session_reconnect COMMAND wfas-session-tests)
