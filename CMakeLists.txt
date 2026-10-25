cmake_minimum_required(VERSION 3.20)
project(szhatie VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SZHATIE_TESTS "Собирать тесты (GoogleTest скачивается автоматически)" ON)
option(SZHATIE_SANITIZE "Собирать с AddressSanitizer и UBSan" OFF)

add_library(szhatie_core src/huffman.cpp src/deflate.cpp src/inflate.cpp src/gzip.cpp)
target_include_directories(szhatie_core PUBLIC src)

if(MSVC)
  target_compile_options(szhatie_core PRIVATE /W4 /permissive-)
  target_compile_options(szhatie_core PUBLIC /utf-8)
else()
  target_compile_options(szhatie_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(SZHATIE_SANITIZE AND NOT MSVC)
  target_compile_options(szhatie_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(szhatie_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(szhatie src/main.cpp)
target_link_libraries(szhatie PRIVATE szhatie_core)

if(SZHATIE_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(szhatie_tests tests/szhatie_test.cpp)
  target_link_libraries(szhatie_tests PRIVATE szhatie_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(szhatie_tests)
endif()
