cmake_minimum_required(VERSION 3.16)
project(cdn_cache_optimizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(cdncore
  src/Knapsack.cpp
  src/Catalog.cpp
  src/Traffic.cpp
  src/Strategies.cpp
  src/Simulation.cpp
  src/Experiments.cpp
)
target_include_directories(cdncore PUBLIC include)
target_link_libraries(cdncore PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(cdncore PRIVATE /W4)
else()
  target_compile_options(cdncore PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cdnopt apps/cdnopt.cpp)
target_link_libraries(cdnopt PRIVATE cdncore)

option(CDN_BUILD_TESTS "Build unit tests" ON)
if(CDN_BUILD_TESTS)
  enable_testing()
  add_executable(cdn_tests tests/test_main.cpp tests/test_knapsack.cpp tests/test_strategies.cpp tests/test_traffic.cpp)
  target_link_libraries(cdn_tests PRIVATE cdncore)
  add_test(NAME cdn_tests COMMAND cdn_tests)
endif()
