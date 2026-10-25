cmake_minimum_required(VERSION 3.20)
project(cpp_concurrency_lab VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# SANITIZE: "address,undefined" or "thread" (ASan and TSan cannot be combined).
set(SANITIZE "" CACHE STRING "Sanitizers to enable: address,undefined | thread | (empty)")
if(SANITIZE)
  message(STATUS "Sanitizers enabled: ${SANITIZE}")
  # Global, so GoogleTest is instrumented the same way as our code.
  add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${SANITIZE})
endif()

find_package(Threads REQUIRED)

add_library(cclab INTERFACE)
target_include_directories(cclab INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(cclab INTERFACE cxx_std_23)
target_link_libraries(cclab INTERFACE Threads::Threads)

function(cclab_warnings tgt)
  target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

# --- tools ---------------------------------------------------------------------
add_executable(race tools/race.cpp)
target_link_libraries(race PRIVATE cclab)
cclab_warnings(race)

add_executable(pipeline_demo tools/pipeline_demo.cpp)
target_link_libraries(pipeline_demo PRIVATE cclab)
cclab_warnings(pipeline_demo)

# --- benchmarks: never under sanitizers (the numbers would be meaningless) -----------
if(NOT SANITIZE)
  add_executable(bench_queues bench/bench_queues.cpp)
  target_link_libraries(bench_queues PRIVATE cclab)
  cclab_warnings(bench_queues)

  add_executable(false_sharing bench/false_sharing.cpp)
  target_link_libraries(false_sharing PRIVATE cclab)
  cclab_warnings(false_sharing)
else()
  message(STATUS "benchmarks are not built under sanitizers")
endif()

# --- tests ---------------------------------------------------------------------
option(CCLAB_BUILD_TESTS "Build the GoogleTest suites" ON)
if(CCLAB_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(cclab_tests
    tests/test_bounded_queue.cpp
    tests/test_latency_recorder.cpp
    tests/test_pipeline.cpp
    tests/test_queues_reference.cpp
    tests/test_spsc.cpp
    tests/test_thread_pool.cpp)
  target_include_directories(cclab_tests PRIVATE tests)
  target_link_libraries(cclab_tests PRIVATE cclab GTest::gtest_main)
  cclab_warnings(cclab_tests)

  include(GoogleTest)
  set(CCLAB_TEST_TIMEOUT 300 CACHE STRING "Per-test timeout in seconds (the mutation check sets 20)")
  gtest_discover_tests(cclab_tests PROPERTIES TIMEOUT ${CCLAB_TEST_TIMEOUT})
endif()
