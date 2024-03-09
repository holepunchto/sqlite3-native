cmake_minimum_required(VERSION 3.25)

project(sqlite_vfs_testing C)

include(bare)

add_bare_module(sqlite_vfs_testing)

target_sources(
  ${sqlite_vfs_testing}
  PRIVATE
    binding.c
    vfs.c
    vendor/sqlite/sqlite3.c
)

target_include_directories(
  ${sqlite_vfs_testing}    
  PUBLIC
    INTERFACE_INCLUDE_DIRECTORIES
    vendor/sqlite
)

