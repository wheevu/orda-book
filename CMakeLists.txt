cmake_minimum_required(VERSION 3.20)
project(orda-book LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOB_ENABLE_ASAN "Enable address sanitizer" OFF)
option(LOB_BUILD_FUZZER "Build the libFuzzer differential target" OFF)

if(LOB_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

add_library(orda_core
  src/order_book.cpp
  src/pooled_order_book.cpp
  src/ladder_order_book.cpp
  src/event_parser.cpp
)

target_include_directories(orda_core PUBLIC src)

target_compile_options(orda_core PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wshadow
)

add_executable(orda_replay src/replay_main.cpp)
target_link_libraries(orda_replay PRIVATE orda_core)

add_executable(orda_benchmark src/benchmark_main.cpp)
target_link_libraries(orda_benchmark PRIVATE orda_core)

add_executable(orda_ingress_benchmark src/ingress_benchmark_main.cpp)
target_link_libraries(orda_ingress_benchmark PRIVATE orda_core Threads::Threads)

add_executable(orda_allocation_benchmark src/allocation_benchmark_main.cpp)
target_link_libraries(orda_allocation_benchmark PRIVATE orda_core)

add_executable(orda_tail_benchmark src/tail_benchmark_main.cpp)
target_link_libraries(orda_tail_benchmark PRIVATE orda_core)

add_executable(orda_tests
  tests/test_main.cpp
  tests/order_book_tests.cpp
  tests/abuse_tests.cpp
  tests/spsc_queue_tests.cpp
  tests/property_tests.cpp
  tests/reference_order_book.cpp
  tests/differential_tests.cpp
  tests/replay_tests.cpp
)
target_link_libraries(orda_tests PRIVATE orda_core)
target_link_libraries(orda_tests PRIVATE Threads::Threads)

include(CTest)
enable_testing()
add_test(NAME orda_tests COMMAND orda_tests)

if(LOB_BUILD_FUZZER)
  add_executable(orda_fuzz
    tests/fuzz_target.cpp
    tests/reference_order_book.cpp
  )
  target_link_libraries(orda_fuzz PRIVATE orda_core)
  target_compile_options(orda_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(orda_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
