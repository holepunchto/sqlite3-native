cmake_minimum_required(VERSION 3.25)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-napi REQUIRED PATHS node_modules/cmake-napi)
find_package(cmake-npm REQUIRED PATHS node_modules/cmake-npm)

project(sqlite3_native C)

fetch_package("https://sqlite.org/2024/sqlite-amalgamation-3470200.zip" SOURCE_DIR sqlite3)

add_library(sqlite3)

target_sources(
  sqlite3
  PRIVATE
    ${sqlite3}/sqlite3.c
)

target_include_directories(
  sqlite3
  INTERFACE
    ${sqlite3}
)

add_bare_module(sqlite3_native_bare)

target_sources(
  ${sqlite3_native_bare}
  PRIVATE
    binding.c
)

target_link_libraries(
  ${sqlite3_native_bare}
  PRIVATE
    sqlite3
)

add_napi_module(sqlite3_native_node)

target_sources(
  ${sqlite3_native_node}
  PRIVATE
    binding.c
)

target_link_libraries(
  ${sqlite3_native_node}
  PUBLIC
    sqlite3
)

resolve_node_module(bare-compat-napi compat)

target_include_directories(
  ${sqlite3_native_node}
  PRIVATE
    "${compat}/include"
)
