# Project configuration
cmake_minimum_required(VERSION 3.11)
project(zara-core VERSION 0.0.0 LANGUAGES CXX)

# C++ configuration
set (CMAKE_CXX_STANDARD 17)
set (CXX_STANDARD 17)
set (CXX_STANDARD_REQUIRED ON)

include_directories("${CMAKE_CURRENT_LIST_DIR}/include/")

# External dependencies
find_package(GTest REQUIRED)
find_package(benchmark REQUIRED)
find_package(CapnProto REQUIRED)
find_package(Boost COMPONENTS system REQUIRED)

add_subdirectory(test)
add_subdirectory(bench)
