## This is just for building the library self-test executable
##
## TODO add an entry and a find script to help people use lmdbcols.hpp.

cmake_minimum_required (VERSION 2.8)

project (lmdbcols)


## ======================================================================
## == Language options - this is a C++11 or higher library

set (CMAKE_CXX_STANDARD 11)
set (CMAKE_CXX_EXTNSIONS OFF)


## ======================================================================
## == run_tests

add_executable (run_tests src/run_tests.cpp)

target_include_directories (run_tests
                            PRIVATE include libs/lmdbxx)

target_link_libraries (run_tests lmdb)

## Mingw/msys requires this, for its mmap-style functions
## TODO check this is still the case
if (MINGW)
  target_link_libraries (run_tests ntdll)
endif()

## TODO add the MSVC equivalent warnings flags
if (CMAKE_COMPILER_IS_GNUCC OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  target_compile_options (run_tests
                          PRIVATE -Wall -Wextra)
endif()
