cmake_minimum_required(VERSION 3.25)

project(sqlite3_native C)

include(bare)

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
