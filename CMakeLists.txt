cmake_minimum_required(VERSION 3.20)
project(distributed_object_store CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# --- Warning policy -----------------------------------------------------------
option(DOS_WERROR "Treat compiler warnings as errors" OFF)

add_library(dos_warnings INTERFACE)
target_compile_options(dos_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(DOS_WERROR)
  target_compile_options(dos_warnings INTERFACE -Werror)
endif()

# --- Sanitizers ---------------------------------------------------------------
# -DDOS_SANITIZER=thread|address|undefined applies the sanitizer to all targets.
set(DOS_SANITIZER "" CACHE STRING "Sanitizer: thread, address, or undefined")
if(DOS_SANITIZER)
  message(STATUS "Building with sanitizer: ${DOS_SANITIZER}")
  add_compile_options(-fsanitize=${DOS_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${DOS_SANITIZER})
endif()

# --- Dependencies -------------------------------------------------------------
find_package(GTest REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

include(CTest)
include(GoogleTest)

# --- Core library -------------------------------------------------------------
add_library(dos_core
  src/common/status.cc
  src/common/sha256.cc
  src/common/digest.cc
  src/common/hash.cc
  src/common/thread_pool.cc
  src/cluster/consistent_hash_ring.cc
  src/cluster/cluster_map.cc
  src/storage/sqlite_metadata_store.cc
  src/storage/local_object_store.cc)
target_include_directories(dos_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${SQLite3_INCLUDE_DIRS})
# Link SQLite via find_package variables rather than an imported target: the
# target name differs across CMake versions (SQLite::SQLite3 vs SQLite3::SQLite3),
# but ${SQLite3_LIBRARIES} is stable.
target_link_libraries(dos_core PUBLIC ${SQLite3_LIBRARIES} Threads::Threads PRIVATE dos_warnings)

# --- Tools --------------------------------------------------------------------
add_executable(storage_demo tools/demo/storage_demo.cc)
target_link_libraries(storage_demo PRIVATE dos_core dos_warnings)

add_executable(rebalance_bench tools/clusterbench/rebalance_bench.cc)
target_link_libraries(rebalance_bench PRIVATE dos_core dos_warnings)

# --- Unit tests ---------------------------------------------------------------
if(BUILD_TESTING)
  add_executable(dos_unit_tests
    tests/unit/sha256_test.cc
    tests/unit/digest_test.cc
    tests/unit/local_object_store_test.cc
    tests/unit/thread_pool_test.cc
    tests/unit/concurrency_test.cc
    tests/unit/hash_test.cc
    tests/unit/consistent_hash_ring_test.cc
    tests/unit/cluster_map_test.cc)
  target_link_libraries(dos_unit_tests PRIVATE dos_core dos_warnings GTest::gtest_main)
  gtest_discover_tests(dos_unit_tests)
endif()
