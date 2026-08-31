cmake_minimum_required(VERSION 3.16)
project(rdb_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(RDB_BUILD_TESTS "Build unit tests" ON)
option(RDB_BUILD_BENCH "Build benchmarks" ON)
option(RDB_WERROR "Treat warnings as errors" OFF)
set(RDB_SANITIZER "none" CACHE STRING "none | address | thread | undefined")

find_package(Threads REQUIRED)

add_library(rdb INTERFACE)
target_include_directories(rdb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdb INTERFACE Threads::Threads)

add_library(rdb_flags INTERFACE)
target_compile_options(rdb_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
if(RDB_WERROR)
  target_compile_options(rdb_flags INTERFACE -Werror)
endif()

# 性能数据必须在 RDB_SANITIZER=none 且 -O2 下测量：ASan 约 2x 慢，TSan 可达 5-15x。
if(NOT RDB_SANITIZER STREQUAL "none")
  if(RDB_SANITIZER STREQUAL "address")
    set(_rdb_san "address,undefined")
  else()
    set(_rdb_san "${RDB_SANITIZER}")
  endif()
  target_compile_options(rdb_flags INTERFACE -fsanitize=${_rdb_san} -fno-omit-frame-pointer -g)
  target_link_options(rdb_flags INTERFACE -fsanitize=${_rdb_san})
endif()

function(rdb_add_test name)
  add_executable(${name} ${ARGN} ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp)
  target_link_libraries(${name} PRIVATE rdb rdb_flags)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

if(RDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RDB_BUILD_BENCH)
  add_subdirectory(bench)
endif()
