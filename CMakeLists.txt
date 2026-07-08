cmake_minimum_required(VERSION 3.15)
project(ToyCCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}/include/frontend
    ${CMAKE_SOURCE_DIR}/include/midend
    ${CMAKE_SOURCE_DIR}/include/backend
    ${CMAKE_SOURCE_DIR}/include/common
)

file(GLOB_RECURSE SOURCES
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

add_executable(compiler ${SOURCES})
target_compile_features(compiler PRIVATE cxx_std_20)
