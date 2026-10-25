cmake_minimum_required(VERSION 3.20)

project(single_header
  VERSION 0.1.0
  DESCRIPTION "Small C++ single-header amalgamation tool"
  LANGUAGES CXX)

option(SINGLE_HEADER_WITH_CLANG "Enable conservative compiler-backed function stitching" OFF)

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
  src/output_cleanup.cpp
  src/stitching.cpp
  src/main.cpp)
target_compile_features(single_header PRIVATE cxx_std_17)
target_link_libraries(single_header PRIVATE nlohmann_json::nlohmann_json)

if(SINGLE_HEADER_WITH_CLANG)
  option(SINGLE_HEADER_CLANG_UNSANITIZED_SDK
    "Match LLVM arena annotations to an unsanitized SDK when enabling ASan" OFF)
  enable_language(C)
  find_package(LLVM REQUIRED CONFIG)
  find_package(Clang REQUIRED CONFIG)
  if(NOT LLVM_VERSION_MAJOR EQUAL 20)
    message(FATAL_ERROR "Function stitching is pinned to LLVM/Clang 20")
  endif()
  target_sources(single_header PRIVATE
    src/stitching/engine.cpp
    src/stitching/frontend.cpp
    src/stitching/semantics.cpp
    src/stitching/source.cpp
    src/stitching/validation.cpp)
  target_include_directories(single_header SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  target_compile_definitions(single_header PRIVATE SINGLE_HEADER_WITH_CLANG=1)
  if(SINGLE_HEADER_CLANG_UNSANITIZED_SDK)
    target_compile_definitions(single_header PRIVATE SINGLE_HEADER_CLANG_UNSANITIZED_SDK=1)
  endif()
  target_compile_definitions(single_header PRIVATE SINGLE_HEADER_CLANG_BINARY="${LLVM_TOOLS_BINARY_DIR}/clang++${CMAKE_EXECUTABLE_SUFFIX}")
  target_link_libraries(single_header PRIVATE clangTooling clangFrontend clangAST clangIndex clangLex)
  if(MSVC)
    target_link_options(single_header PRIVATE /STACK:33554432)
    # Clang's heavily instantiated visitor emits spurious C4702 under MSVC.
    set_source_files_properties(
      src/stitching/frontend.cpp src/stitching/semantics.cpp
      PROPERTIES COMPILE_OPTIONS /wd4702)
  endif()
endif()

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

  add_executable(single_header_tests tests/test_cli.cpp tests/test_output_cleanup.cpp src/output_cleanup.cpp)
  target_compile_features(single_header_tests PRIVATE cxx_std_17)
  target_link_libraries(single_header_tests PRIVATE Catch2::Catch2WithMain)
  target_link_libraries(single_header_tests PRIVATE nlohmann_json::nlohmann_json)
  if(SINGLE_HEADER_WITH_CLANG)
    target_sources(single_header_tests PRIVATE tests/test_stitching.cpp)
    target_compile_definitions(single_header_tests PRIVATE SINGLE_HEADER_TEST_WITH_CLANG=1)
  endif()
  target_compile_definitions(single_header_tests PRIVATE
    SINGLE_HEADER_TEST_TOOL="$<TARGET_FILE:single_header>"
    SINGLE_HEADER_TEST_FIXTURES="${TEST_FIXTURE_DIR}"
    SINGLE_HEADER_TEST_OUTPUT_DIR="${TEST_OUTPUT_DIR}/$<CONFIG>"
    SINGLE_HEADER_TEST_CMAKE_COMMAND="${CMAKE_COMMAND}"
    SINGLE_HEADER_TEST_CMAKE_GENERATOR="${CMAKE_GENERATOR}")

  catch_discover_tests(single_header_tests)
endif()
