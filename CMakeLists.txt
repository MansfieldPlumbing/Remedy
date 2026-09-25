cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0091 NEW)

project(Revoke LANGUAGES CXX)

set(
    CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Interface library for header-only components
add_library(revoke_headers INTERFACE)
target_include_directories(revoke_headers INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/native/core"
)

# Active Compiled Targets
add_executable(revoke_echo_worker
    src/host/win32/revoke_echo_worker.cpp
)
target_link_libraries(revoke_echo_worker PRIVATE revoke_headers)

add_executable(test_codec_golden
    tests/native/test_codec_golden.cpp
)
target_link_libraries(test_codec_golden PRIVATE revoke_headers)

add_test(NAME test_codec_golden COMMAND test_codec_golden)

add_executable(test_object_table
    tests/native/test_object_table.cpp
    src/native/core/object_table.cpp
)
target_link_libraries(test_object_table PRIVATE revoke_headers)
add_test(NAME test_object_table COMMAND test_object_table)

add_executable(test_object_table_concurrent
    tests/native/test_object_table_concurrent.cpp
    src/native/core/object_table.cpp
)
target_link_libraries(test_object_table_concurrent PRIVATE revoke_headers)
add_test(NAME test_object_table_concurrent COMMAND test_object_table_concurrent)
set_tests_properties(test_object_table_concurrent PROPERTIES TIMEOUT 10)

add_library(revoke_worker_win32 STATIC
    src/native/platform/win32/worker_win32.cpp
)
target_link_libraries(revoke_worker_win32 PRIVATE revoke_headers)

add_executable(revoke_worker_fixture
    src/host/win32/revoke_worker_fixture.cpp
)
target_link_libraries(revoke_worker_fixture PRIVATE revoke_headers)

add_executable(test_worker_start_containment
    tests/native/test_worker_start_containment.cpp
    src/native/platform/win32/worker_win32.cpp
)
target_link_libraries(test_worker_start_containment PRIVATE revoke_headers)
target_compile_definitions(test_worker_start_containment PRIVATE REVOKE_TEST_STAGED_FAILURE_SEAM)
add_test(NAME test_worker_start_containment COMMAND test_worker_start_containment $<TARGET_FILE:revoke_worker_fixture>)
set_tests_properties(test_worker_start_containment PROPERTIES TIMEOUT 15)

add_executable(test_worker_token_rundown
    tests/native/test_worker_token_rundown.cpp
    src/native/platform/win32/worker_win32.cpp
)
target_link_libraries(test_worker_token_rundown PRIVATE revoke_headers)
target_compile_definitions(test_worker_token_rundown PRIVATE REVOKE_TEST_WORKER_RUNDOWN_SEAM)
add_test(NAME test_worker_token_rundown COMMAND test_worker_token_rundown $<TARGET_FILE:revoke_worker_fixture>)
set_tests_properties(test_worker_token_rundown PROPERTIES TIMEOUT 25)

add_executable(test_worker_tree_containment
    tests/native/test_worker_tree_containment.cpp
    src/native/platform/win32/worker_win32.cpp
)
target_link_libraries(test_worker_tree_containment PRIVATE revoke_headers)
target_compile_definitions(test_worker_tree_containment PRIVATE REVOKE_TEST_WORKER_TREE_SEAM)
add_test(NAME test_worker_tree_containment COMMAND test_worker_tree_containment $<TARGET_FILE:revoke_worker_fixture>)
set_tests_properties(test_worker_tree_containment PROPERTIES TIMEOUT 60)

add_library(revoke_channel_win32 STATIC
    src/native/platform/win32/channel_win32.cpp
)
target_link_libraries(revoke_channel_win32 PRIVATE revoke_headers)

add_executable(test_channel_lifetime
    tests/native/test_channel_lifetime.cpp
    src/native/platform/win32/channel_win32.cpp
)
target_link_libraries(test_channel_lifetime PRIVATE revoke_headers)
target_compile_definitions(test_channel_lifetime PRIVATE REVOKE_TEST_CHANNEL_LIFETIME_SEAM)
add_test(NAME test_channel_lifetime COMMAND test_channel_lifetime)
set_tests_properties(test_channel_lifetime PROPERTIES TIMEOUT 480)

# Explicitly Deferred Recovered Sources (Require semantic repair or missing implementations):
# - tests/native/test_collapse.cpp (missing domain.h header)
# - tests/native/test_echo_slice.cpp (missing domain.h header)
# - src/managed/Revoke.EchoWorker/Program.cs (C# managed runtime deferred)
# - src/managed/Revoke.EchoWorker/Revoke.EchoWorker.csproj (C# managed project deferred)
