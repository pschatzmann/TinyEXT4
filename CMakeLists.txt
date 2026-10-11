cmake_minimum_required(VERSION 3.16)

# TinyEXT4: build and test the file system on the desktop (Linux, macOS).
#
#   cmake -B build && cmake --build build && ctest --test-dir build
#
# The tests need e2fsprogs (mke2fs, e2fsck, debugfs).
project(TinyEXT4 LANGUAGES C CXX)

option(TINYEXT4_BUILD_TESTS "Build the ctest suite" ON)
option(TINYEXT4_BUILD_EXAMPLES "Build the desktop examples" ON)
option(TINYEXT4_SANITIZE "Build with address and undefined behavior sanitizer" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# header only library
add_library(tinyext4 INTERFACE)
target_include_directories(tinyext4 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(tinyext4 INTERFACE -Wall -Wextra)

if(TINYEXT4_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(TINYEXT4_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TINYEXT4_BUILD_EXAMPLES)
  add_subdirectory(examples/desktop)
endif()
