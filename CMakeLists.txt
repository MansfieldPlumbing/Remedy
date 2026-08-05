cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0091 NEW)

project(Remedy LANGUAGES CXX)

set(
    CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Interface library for header-only components
add_library(remedy_headers INTERFACE)
target_include_directories(remedy_headers INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/native/core"
)

# Active Compiled Targets
add_executable(remedy_echo_worker
    src/host/win32/remedy_echo_worker.cpp
)
target_link_libraries(remedy_echo_worker PRIVATE remedy_headers)

add_executable(test_codec_golden
    tests/native/test_codec_golden.cpp
)
target_link_libraries(test_codec_golden PRIVATE remedy_headers)

add_test(NAME test_codec_golden COMMAND test_codec_golden)

# Explicitly Deferred Recovered Sources (Require semantic repair or missing implementations):
# - src/native/core/object_table.cpp (missing domain_object::~domain_object implementation)
# - src/native/platform/win32/channel_win32.cpp (platform worker/channel integration deferred)
# - src/native/platform/win32/worker_win32.cpp (platform worker/channel integration deferred)
# - tests/native/test_collapse.cpp (missing domain.h header)
# - tests/native/test_echo_slice.cpp (missing domain.h header)
# - tests/native/test_object_table.cpp (constructor signature mismatch C2665)
# - tests/native/test_object_table_concurrent.cpp (unresolved external domain_object::~domain_object)
# - src/managed/Remedy.EchoWorker/Program.cs (C# managed runtime deferred)
# - src/managed/Remedy.EchoWorker/Remedy.EchoWorker.csproj (C# managed project deferred)
