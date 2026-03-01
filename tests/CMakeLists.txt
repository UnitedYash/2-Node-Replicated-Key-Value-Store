cmake_minimum_required(VERSION 3.14)
project(kvstore_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch Google Test automatically — no manual install needed.
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(run_tests
    test_kv_store.cpp
    test_wal.cpp
    test_thread_pool.cpp
)

target_include_directories(run_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../server
)

target_compile_options(run_tests PRIVATE -Wall -Wextra -O2)
target_link_libraries(run_tests PRIVATE GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(run_tests)
