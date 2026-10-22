cmake_minimum_required(VERSION 3.20)
project(intrusive_list_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ILIST_SANITIZE "" CACHE STRING "Sanitizers: address,undefined or thread")
if(ILIST_SANITIZE)
  add_compile_options(-fsanitize=${ILIST_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${ILIST_SANITIZE})
endif()

add_library(ilist INTERFACE)
target_include_directories(ilist INTERFACE include)

find_package(Threads REQUIRED)
enable_testing()
add_executable(test_list tests/test_list.cpp)
target_link_libraries(test_list PRIVATE ilist Threads::Threads)
target_compile_options(test_list PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
add_test(NAME list COMMAND test_list)
