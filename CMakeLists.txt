cmake_minimum_required(VERSION 3.16)
project(simdjson_arrow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SIMDJSON_ARROW_TESTS "Build the tests" ON)
option(SIMDJSON_ARROW_BENCHMARKS "Build the benchmarks" ON)
# Arrow C++'s own JSON reader, for comparison: needs Arrow C++ (with ARROW_JSON),
# found by find_package (set CMAKE_PREFIX_PATH to its installation).
option(SIMDJSON_ARROW_BENCHMARK_ARROW_CPP "Build the Arrow C++ benchmark" OFF)

include(FetchContent)
FetchContent_Declare(simdjson
  URL https://github.com/simdjson/simdjson/archive/refs/tags/v5.0.7.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson)

# zmij (third_party/zmij) writes floating-point numbers.
add_library(zmij OBJECT third_party/zmij/zmij.cc)

set(SIMDJSON_ARROW_SOURCES src/simdjson_arrow.cpp src/ndjson_writer.cpp $<TARGET_OBJECTS:zmij>)

add_library(simdjson_arrow STATIC ${SIMDJSON_ARROW_SOURCES})
target_include_directories(simdjson_arrow PUBLIC include PRIVATE third_party/zmij)
target_link_libraries(simdjson_arrow PUBLIC simdjson::simdjson)

# A shared library with the C entry point, for bindings and the Python tests (ctypes).
add_library(simdjson_arrow_c SHARED ${SIMDJSON_ARROW_SOURCES})
target_include_directories(simdjson_arrow_c PUBLIC include PRIVATE third_party/zmij)
target_link_libraries(simdjson_arrow_c PRIVATE simdjson::simdjson)

if(SIMDJSON_ARROW_TESTS)
  enable_testing()
  add_executable(basic_tests tests/basic_tests.cpp)
  target_link_libraries(basic_tests PRIVATE simdjson_arrow)
  add_test(NAME basic_tests COMMAND basic_tests)
endif()

if(SIMDJSON_ARROW_BENCHMARKS)
  add_executable(bench_ndjson benchmarks/bench_ndjson.cpp)
  target_link_libraries(bench_ndjson PRIVATE simdjson_arrow)
  add_executable(bench_write benchmarks/bench_write.cpp)
  target_link_libraries(bench_write PRIVATE simdjson_arrow)
endif()

if(SIMDJSON_ARROW_BENCHMARK_ARROW_CPP)
  find_package(Arrow REQUIRED)
  add_executable(bench_arrow_cpp benchmarks/bench_arrow_cpp.cpp)
  set_target_properties(bench_arrow_cpp PROPERTIES CXX_STANDARD 20)
  target_link_libraries(bench_arrow_cpp PRIVATE Arrow::arrow_shared)
endif()
