# xexdump: loads an XEX through the ReXGlue runtime and dumps each section
# as a raw big-endian binary, for disassembly with powerpc objdump.
cmake_minimum_required(VERSION 3.25)
project(xexdump LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(rexglue REQUIRED CONFIG)
add_executable(xexdump xexdump.cpp)
target_link_libraries(xexdump PRIVATE rex::runtime)
