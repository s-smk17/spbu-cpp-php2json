cmake_minimum_required(VERSION 3.10)
project(php2json CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/utf-8 /W4)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

add_executable(php2json src/main.cpp src/lexer.cpp src/parser.cpp)
