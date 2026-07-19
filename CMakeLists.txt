cmake_minimum_required(VERSION 3.25)
project(daichod VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The engine version this binary is built against; the Containerfile's
# GNUCASH_VERSION arg is the single source of truth and passes it through.
set(GNUCASH_PREFIX "/opt/gnucash" CACHE PATH "GnuCash install prefix")
set(GNUCASH_PINNED_VERSION "$ENV{GNUCASH_VERSION}" CACHE STRING "Pinned GnuCash version")
if(NOT GNUCASH_PINNED_VERSION)
  set(GNUCASH_PINNED_VERSION "unknown")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)

find_program(PROTOC protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

# ------------------------------------------------------------ proto codegen
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
add_custom_command(
  OUTPUT ${PROTO_GEN_DIR}/shim.pb.cc ${PROTO_GEN_DIR}/shim.pb.h
         ${PROTO_GEN_DIR}/shim.grpc.pb.cc ${PROTO_GEN_DIR}/shim.grpc.pb.h
  COMMAND ${PROTOC}
          --proto_path=${PROTO_DIR}
          --proto_path=/usr/include
          --cpp_out=${PROTO_GEN_DIR}
          --grpc_out=${PROTO_GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          ${PROTO_DIR}/shim.proto
  DEPENDS ${PROTO_DIR}/shim.proto
  COMMENT "Generating C++ stubs for daicho.shim.v1")

# google.rpc.Status is serialized into grpc-status-details-bin; the proto is
# vendored because Ubuntu's protobuf packages do not ship it.
add_custom_command(
  OUTPUT ${PROTO_GEN_DIR}/google/rpc/status.pb.cc
         ${PROTO_GEN_DIR}/google/rpc/status.pb.h
  COMMAND ${PROTOC}
          --proto_path=${PROTO_DIR}
          --proto_path=/usr/include
          --cpp_out=${PROTO_GEN_DIR}
          ${PROTO_DIR}/google/rpc/status.proto
  DEPENDS ${PROTO_DIR}/google/rpc/status.proto
  COMMENT "Generating google.rpc.Status")

# daichod.internal.Outcome — journal records, never on the wire.
add_custom_command(
  OUTPUT ${PROTO_GEN_DIR}/internal.pb.cc ${PROTO_GEN_DIR}/internal.pb.h
  COMMAND ${PROTOC}
          --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/src/journal
          --proto_path=${PROTO_DIR}
          --proto_path=/usr/include
          --cpp_out=${PROTO_GEN_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src/journal/internal.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/journal/internal.proto
          ${PROTO_DIR}/shim.proto
  COMMENT "Generating daichod.internal journal records")

add_library(daichod_proto STATIC
  ${PROTO_GEN_DIR}/shim.pb.cc
  ${PROTO_GEN_DIR}/shim.grpc.pb.cc
  ${PROTO_GEN_DIR}/google/rpc/status.pb.cc
  ${PROTO_GEN_DIR}/internal.pb.cc)
target_include_directories(daichod_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(daichod_proto PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF)

# ------------------------------------------------------------ gnucash engine
# GnuCash exports no CMake/pkg-config metadata; locate the installed libs.
find_library(GNC_ENGINE_LIB gnc-engine
  PATHS ${GNUCASH_PREFIX}/lib ${GNUCASH_PREFIX}/lib/gnucash REQUIRED)
find_library(GNC_CORE_UTILS_LIB gnc-core-utils
  PATHS ${GNUCASH_PREFIX}/lib ${GNUCASH_PREFIX}/lib/gnucash REQUIRED)
get_filename_component(GNC_LIB_DIR ${GNC_ENGINE_LIB} DIRECTORY)

add_library(gnucash_engine INTERFACE)
target_include_directories(gnucash_engine INTERFACE ${GNUCASH_PREFIX}/include/gnucash)
target_link_libraries(gnucash_engine INTERFACE
  ${GNC_ENGINE_LIB} ${GNC_CORE_UTILS_LIB} PkgConfig::GLIB)
target_link_directories(gnucash_engine INTERFACE ${GNC_LIB_DIR})
# Engine modules (backends) load at runtime from the install tree.
target_link_options(gnucash_engine INTERFACE
  "-Wl,-rpath,${GNC_LIB_DIR}" "-Wl,-rpath,${GNUCASH_PREFIX}/lib/gnucash")

# ------------------------------------------------------------------ daichod
configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/gen/daichod_version.h)

# Everything except main(), shared with the tools and tests.
add_library(daichod_core STATIC
  src/engine/map.cpp
  src/engine/session.cpp
  src/engine/worker.cpp
  src/journal/journal.cpp
  src/rpc/account_service.cpp
  src/rpc/commodity_service.cpp
  src/rpc/error.cpp
  src/rpc/mutation.cpp
  src/rpc/session_service.cpp
  src/rpc/transaction_service.cpp)
target_include_directories(daichod_core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(daichod_core
  PUBLIC daichod_proto gnucash_engine PkgConfig::SQLITE)

add_executable(daichod src/main.cpp)
target_link_libraries(daichod PRIVATE daichod_core)

add_executable(daichod-mkbook tools/mkbook/mkbook.cpp)
target_link_libraries(daichod-mkbook PRIVATE daichod_core)

# -------------------------------------------------------------------- tests
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(test)
endif()
