cmake_minimum_required(VERSION 3.25)

project(bare_sqlite3 C)

include(bare)

add_bare_module(bare_sqlite3)

target_sources(
  ${bare_sqlite3}
  PRIVATE
    binding.c
    vfs.c
    vendor/sqlite/sqlite3.c
)

target_include_directories(
  ${bare_sqlite3}    
  PUBLIC
    INTERFACE_INCLUDE_DIRECTORIES
    vendor/sqlite
)

