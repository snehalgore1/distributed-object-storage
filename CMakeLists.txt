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
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

# gRPC + Protobuf via pkg-config (portable across Homebrew and apt, which do not
# both ship CMake config packages). Codegen uses the protoc/plugin executables.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_program(PROTOC protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

# --- Generated protobuf/gRPC code ---------------------------------------------
set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(PROTO_GEN
  ${PROTO_GEN_DIR}/storage.pb.cc  ${PROTO_GEN_DIR}/storage.pb.h
  ${PROTO_GEN_DIR}/storage.grpc.pb.cc  ${PROTO_GEN_DIR}/storage.grpc.pb.h
  ${PROTO_GEN_DIR}/metadata.pb.cc  ${PROTO_GEN_DIR}/metadata.pb.h
  ${PROTO_GEN_DIR}/metadata.grpc.pb.cc  ${PROTO_GEN_DIR}/metadata.grpc.pb.h)
add_custom_command(
  OUTPUT ${PROTO_GEN}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}
  COMMAND ${PROTOC}
    --proto_path=${PROTO_SRC_DIR}
    --cpp_out=${PROTO_GEN_DIR}
    --grpc_out=${PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
    ${PROTO_SRC_DIR}/storage.proto ${PROTO_SRC_DIR}/metadata.proto
  DEPENDS ${PROTO_SRC_DIR}/storage.proto ${PROTO_SRC_DIR}/metadata.proto
  COMMENT "Generating gRPC/protobuf code from proto/*.proto"
  VERBATIM)

# dos_proto: generated code only. Not built with the strict warning policy since
# generated sources are not ours to lint.
add_library(dos_proto
  ${PROTO_GEN_DIR}/storage.pb.cc ${PROTO_GEN_DIR}/storage.grpc.pb.cc
  ${PROTO_GEN_DIR}/metadata.pb.cc ${PROTO_GEN_DIR}/metadata.grpc.pb.cc)
target_include_directories(dos_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(dos_proto PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF Threads::Threads)

include(CTest)  # defines BUILD_TESTING (default ON)

# --- Core library -------------------------------------------------------------
add_library(dos_core
  src/common/status.cc
  src/common/sha256.cc
  src/common/digest.cc
  src/common/hash.cc
  src/common/thread_pool.cc
  src/common/metrics.cc
  src/common/logging.cc
  src/cluster/consistent_hash_ring.cc
  src/cluster/cluster_map.cc
  src/cluster/failure_detector.cc
  src/cluster/metadata_repository.cc
  src/storage/sqlite_metadata_store.cc
  src/storage/wal.cc
  src/storage/local_object_store.cc)
target_include_directories(dos_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${SQLite3_INCLUDE_DIRS})
# Link SQLite via find_package variables rather than an imported target: the
# target name differs across CMake versions (SQLite::SQLite3 vs SQLite3::SQLite3),
# but ${SQLite3_LIBRARIES} is stable.
target_link_libraries(dos_core PUBLIC ${SQLite3_LIBRARIES} Threads::Threads PRIVATE dos_warnings)

# --- RPC layer (server, client, coordinator) ----------------------------------
add_library(dos_rpc
  src/network/storage_node_server.cc
  src/network/storage_node_client.cc
  src/network/coordinator.cc
  src/network/repairer.cc
  src/network/metadata_server.cc
  src/network/metadata_client.cc
  src/network/http_server.cc
  src/network/http_client.cc
  src/network/http_gateway.cc)
target_include_directories(dos_rpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dos_rpc PUBLIC dos_core dos_proto PRIVATE dos_warnings)

# --- Tools --------------------------------------------------------------------
add_executable(storage_demo tools/demo/storage_demo.cc)
target_link_libraries(storage_demo PRIVATE dos_core dos_warnings)

add_executable(rebalance_bench tools/clusterbench/rebalance_bench.cc)
target_link_libraries(rebalance_bench PRIVATE dos_core dos_warnings)

add_executable(dos_node tools/node/node_main.cc)
target_link_libraries(dos_node PRIVATE dos_rpc dos_warnings)

add_executable(dos_metadata tools/metadata/metadata_main.cc)
target_link_libraries(dos_metadata PRIVATE dos_rpc dos_warnings)

add_executable(dos_gateway tools/gateway/gateway_main.cc)
target_link_libraries(dos_gateway PRIVATE dos_rpc dos_warnings)

add_executable(dos_cli tools/cli/cli_main.cc)
target_link_libraries(dos_cli PRIVATE dos_rpc dos_warnings)

add_executable(micro_bench tools/bench/micro_bench.cc)
target_link_libraries(micro_bench PRIVATE dos_core dos_warnings)

add_executable(loadgen tools/bench/loadgen.cc)
target_link_libraries(loadgen PRIVATE dos_rpc dos_warnings)

add_executable(cluster_demo tools/demo/cluster_demo.cc)
target_link_libraries(cluster_demo PRIVATE dos_rpc dos_warnings)

# --- Unit tests ---------------------------------------------------------------
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(dos_unit_tests
    tests/unit/sha256_test.cc
    tests/unit/digest_test.cc
    tests/unit/local_object_store_test.cc
    tests/unit/thread_pool_test.cc
    tests/unit/concurrency_test.cc
    tests/unit/hash_test.cc
    tests/unit/consistent_hash_ring_test.cc
    tests/unit/cluster_map_test.cc
    tests/unit/versioning_test.cc
    tests/unit/failure_detector_test.cc
    tests/unit/wal_test.cc
    tests/unit/crash_recovery_test.cc
    tests/unit/metadata_repository_test.cc
    tests/unit/lru_cache_test.cc
    tests/unit/caching_metadata_view_test.cc
    tests/unit/metrics_test.cc
    tests/unit/logging_test.cc)
  target_link_libraries(dos_unit_tests PRIVATE dos_core dos_warnings GTest::gtest_main)
  gtest_discover_tests(dos_unit_tests)

  add_executable(dos_integration_tests
    tests/integration/replication_test.cc
    tests/integration/metadata_split_test.cc
    tests/integration/http_gateway_test.cc
    tests/integration/chaos_test.cc)
  target_link_libraries(dos_integration_tests PRIVATE dos_rpc dos_warnings GTest::gtest_main)
  gtest_discover_tests(dos_integration_tests)
endif()
