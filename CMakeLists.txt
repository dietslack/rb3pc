# rb3pc - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(rb3pc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# The recompiled code must be optimized: unoptimized, the game's audio mixer
# alone needs a whole core and audio underruns (crackling), and everything else
# runs several times slower. A build tree configured with empty Release flags
# (CMAKE_CXX_FLAGS_RELEASE="") gets -O2 here rather than silently building -O0.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "-O[1-3s]")
    message(WARNING "CMAKE_CXX_FLAGS_RELEASE has no optimization level; adding -O2")
    add_compile_options(-O2)
endif()

# Sources
set(RB3PC_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(rb3pc WIN32 ${RB3PC_SOURCES})
else()
    add_executable(rb3pc ${RB3PC_SOURCES})
endif()

rexglue_setup_target(rb3pc)
