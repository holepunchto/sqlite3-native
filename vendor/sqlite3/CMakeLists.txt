cmake_minimum_required(VERSION 3.25)

project(sqlite3 C)

add_library(sqlite3)

target_sources(
  sqlite3
  PRIVATE
    sqlite3.c
)

target_include_directories(
  sqlite3
  PUBLIC
    .
)
