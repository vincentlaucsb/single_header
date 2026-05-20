cmake_minimum_required(VERSION 3.20)

project(single_header
  VERSION 0.1.0
  DESCRIPTION "Small C++ single-header amalgamation tool"
  LANGUAGES CXX)

include(FetchContent)

find_package(nlohmann_json 3 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_executable(single_header
  src/amalgamator.cpp
  src/cli.cpp
  src/config.cpp
  src/helpers.cpp
  src/main.cpp)
target_compile_features(single_header PRIVATE cxx_std_17)
target_link_libraries(single_header PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
  target_compile_options(single_header PRIVATE /W4 /permissive-)
else()
  target_compile_options(single_header PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)

if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG QUIET)
  if(NOT Catch2_FOUND)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.7.1)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  endif()

  include(Catch)

  set(TEST_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  set(TEST_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-output")

  add_executable(single_header_tests tests/test_cli.cpp)
  target_compile_features(single_header_tests PRIVATE cxx_std_17)
  target_link_libraries(single_header_tests PRIVATE Catch2::Catch2WithMain)
  target_compile_definitions(single_header_tests PRIVATE
    SINGLE_HEADER_TEST_TOOL="$<TARGET_FILE:single_header>"
    SINGLE_HEADER_TEST_FIXTURES="${TEST_FIXTURE_DIR}"
    SINGLE_HEADER_TEST_OUTPUT_DIR="${TEST_OUTPUT_DIR}/$<CONFIG>"
    SINGLE_HEADER_TEST_CMAKE_COMMAND="${CMAKE_COMMAND}"
    SINGLE_HEADER_TEST_CMAKE_GENERATOR="${CMAKE_GENERATOR}")

  catch_discover_tests(single_header_tests)
endif()
