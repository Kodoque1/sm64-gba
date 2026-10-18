# Headless NanoBoyAdvance runner (make bench-check): builds NBA's emulator
# core from the fetched source (NBA_DIR, see the Makefile) and links the
# runner against it. NBA's own frontends aren't built.
cmake_minimum_required(VERSION 3.24)
project(nba_runner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${NBA_DIR}/thirdparty thirdparty EXCLUDE_FROM_ALL)
add_subdirectory(${NBA_DIR}/src/nba nba EXCLUDE_FROM_ALL)

add_executable(nba_runner nba_runner.cc)
target_include_directories(nba_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../demo)
target_link_libraries(nba_runner PRIVATE nba)
