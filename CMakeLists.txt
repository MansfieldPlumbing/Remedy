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

add_executable(test_object_table
    tests/native/test_object_table.cpp
    src/native/core/object_table.cpp
)
target_link_libraries(test_object_table PRIVATE remedy_headers)
add_test(NAME test_object_table COMMAND test_object_table)

add_executable(test_object_table_concurrent
    tests/native/test_object_table_concurrent.cpp
    src/native/core/object_table.cpp
)
target_link_libraries(test_object_table_concurrent PRIVATE remedy_headers)
add_test(NAME test_object_table_concurrent COMMAND test_object_table_concurrent)
set_tests_properties(test_object_table_concurrent PROPERTIES TIMEOUT 10)

add_library(remedy_worker_win32 STATIC
    src/native/platform/win32/worker_win32.cpp
)
target_link_libraries(remedy_worker_win32 PRIVATE remedy_headers)

add_executable(remedy_worker_fixture
    src/host/win32/remedy_worker_fixture.cpp
)
target_link_libraries(remedy_worker_fixture PRIVATE remedy_headers)

add_executable(test_worker_start_containment
    tests/native/test_worker_start_containment.cpp
    src/native/platform/win32/worker_win32.cpp
)
target_link_libraries(test_worker_start_containment PRIVATE remedy_headers)
target_compile_definitions(test_worker_start_containment PRIVATE REMEDY_TEST_STAGED_FAILURE_SEAM)
add_test(NAME test_worker_start_containment COMMAND test_worker_start_containment $<TARGET_FILE:remedy_worker_fixture>)
set_tests_properties(test_worker_start_containment PROPERTIES TIMEOUT 15)

# Explicitly Deferred Recovered Sources (Require semantic repair or missing implementations):
# - src/native/platform/win32/channel_win32.cpp (platform worker/channel integration deferred)
# - tests/native/test_collapse.cpp (missing domain.h header)
# - tests/native/test_echo_slice.cpp (missing domain.h header)
# - src/managed/Remedy.EchoWorker/Program.cs (C# managed runtime deferred)
# - src/managed/Remedy.EchoWorker/Remedy.EchoWorker.csproj (C# managed project deferred)
