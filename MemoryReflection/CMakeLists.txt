cmake_minimum_required(VERSION 3.16)
project(memrefl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MEMREFL_SANITIZE "" CACHE STRING "Sanitizer for tests/examples: address, undefined, thread or empty")

find_package(Threads REQUIRED)

add_library(memrefl INTERFACE)
target_include_directories(memrefl INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(memrefl INTERFACE Threads::Threads)

# Примеры и тесты собираются, только когда memrefl — основной проект, а не
# подключён через add_subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(MEMREFL_IS_TOP_LEVEL ON)
else()
  set(MEMREFL_IS_TOP_LEVEL OFF)
endif()
option(MEMREFL_BUILD_EXAMPLES_AND_TESTS "Build memrefl examples and tests" ${MEMREFL_IS_TOP_LEVEL})

function(memrefl_target name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE memrefl)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(MEMREFL_SANITIZE)
    target_compile_options(${name} PRIVATE -fsanitize=${MEMREFL_SANITIZE} -g -fno-omit-frame-pointer)
    target_link_options(${name} PRIVATE -fsanitize=${MEMREFL_SANITIZE})
  endif()
endfunction()

if(MEMREFL_BUILD_EXAMPLES_AND_TESTS)
  memrefl_target(scan_demo examples/scan_demo.cpp)
  memrefl_target(scan_benchmark examples/scan_benchmark.cpp)
  memrefl_target(resolver_demo examples/resolver_demo.cpp)
  memrefl_target(property_demo examples/property_demo.cpp)

  enable_testing()
  memrefl_target(signature_scanner_tests tests/signature_scanner_tests.cpp)
  add_test(NAME signature_scanner_tests COMMAND signature_scanner_tests)

  # Те же тесты без SSE2: проверяет запасной путь (memchr) на любой платформе.
  memrefl_target(signature_scanner_tests_scalar tests/signature_scanner_tests.cpp)
  target_compile_definitions(signature_scanner_tests_scalar PRIVATE MEMREFL_NO_SIMD)
  add_test(NAME signature_scanner_tests_scalar COMMAND signature_scanner_tests_scalar)

  memrefl_target(data_resolver_tests tests/data_resolver_tests.cpp)
  add_test(NAME data_resolver_tests COMMAND data_resolver_tests)

  memrefl_target(property_fetcher_tests tests/property_fetcher_tests.cpp)
  add_test(NAME property_fetcher_tests COMMAND property_fetcher_tests)

  # Сборка проверяет, что заголовки не ломаются макросами <windows.h> (min/max и др.).
  memrefl_target(windows_macros_check tests/windows_macros_check.cpp)
  add_test(NAME windows_macros_check COMMAND windows_macros_check)
endif()
