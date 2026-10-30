cmake_minimum_required(VERSION 3.16)
project(cmdq LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMDQ_SANITIZE "" CACHE STRING "Sanitizer for tests/examples: thread, address or empty")

find_package(Threads REQUIRED)

add_library(cmdq INTERFACE)
target_include_directories(cmdq INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cmdq INTERFACE Threads::Threads)

function(cmdq_target name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE cmdq)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(CMDQ_SANITIZE)
    target_compile_options(${name} PRIVATE -fsanitize=${CMDQ_SANITIZE} -g -fno-omit-frame-pointer)
    target_link_options(${name} PRIVATE -fsanitize=${CMDQ_SANITIZE})
  endif()
endfunction()

cmdq_target(game_loop_demo examples/game_loop_demo.cpp)

enable_testing()
cmdq_target(command_queue_tests tests/command_queue_tests.cpp)
add_test(NAME command_queue_tests COMMAND command_queue_tests)
