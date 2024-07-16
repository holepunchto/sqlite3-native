cmake_minimum_required(VERSION 3.25)

project(sqlite3_native C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(bare)
include(napi)

if(NOT TARGET sqlite3)
  add_subdirectory(vendor/sqlite3 EXCLUDE_FROM_ALL)
endif()

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

bare_include_directories(compat NAPI)

target_include_directories(
  ${sqlite3_native_node}
  PRIVATE
    ${compat}
)

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
