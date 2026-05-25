cmake_minimum_required(VERSION 3.16)
project(twoleft_hashmap LANGUAGES CXX)

add_library(twoleft_hashmap INTERFACE)
target_include_directories(twoleft_hashmap INTERFACE include)
target_compile_features(twoleft_hashmap INTERFACE cxx_std_20)

add_executable(twoleft_demo examples/demo.cpp)
target_link_libraries(twoleft_demo PRIVATE twoleft_hashmap)

add_executable(twoleft_benchmark_int benchmarks/benchmark_int.cpp)
target_link_libraries(twoleft_benchmark_int PRIVATE twoleft_hashmap)

add_executable(twoleft_basic_tests tests/basic_tests.cpp)
target_link_libraries(twoleft_basic_tests PRIVATE twoleft_hashmap)

# Optional documentation target. If Doxygen is installed, run:
#
#   cmake --build build --target docs
#
# This generates HTML documentation under docs/html.
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME "TwoLeftHashMap")
    set(DOXYGEN_PROJECT_BRIEF "Experimental C++20 2-left hash map")
    set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs")
    set(DOXYGEN_EXTRACT_PRIVATE YES)
    set(DOXYGEN_EXTRACT_STATIC YES)
    set(DOXYGEN_MARKDOWN_SUPPORT YES)
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

    doxygen_add_docs(
        docs
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/architecture.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        COMMENT "Generate Doxygen documentation"
    )
endif()
