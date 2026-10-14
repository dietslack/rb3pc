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
    src/debug_hooks.cpp
    src/app_hooks.cpp
    src/screenshot.cpp
    src/instrument_input.cpp
    src/xekeys.cpp
    src/shm_cleanup.cpp
    src/input_bindings.cpp
    src/bindings_dialog.cpp
    src/boot_speed.cpp
    src/frame_stats.cpp
    src/display_dialog.cpp
    src/title_icon.cpp
    src/loading_screen.cpp
    src/rb3_assets.cpp
    src/rb3_ui.cpp
    src/save_tweaks.cpp
    src/gpu_wait.cpp
    src/launcher.cpp
    src/profile.cpp
    src/script_patches.cpp
    src/deluxe.cpp
    src/mouse_menus.cpp
    src/audio_options.cpp
    src/calibration_fps.cpp
    src/shader_warmup.cpp
    src/ark.cpp
    src/dtb.cpp
    src/sha1.cpp
    src/xam_trace.cpp
    src/kernel_trace.cpp
)

if(WIN32)
    add_executable(rb3pc WIN32 ${RB3PC_SOURCES})
else()
    add_executable(rb3pc ${RB3PC_SOURCES})
endif()

rexglue_setup_target(rb3pc)

# zlib: inflating .milo archives for the loading screen's game assets.
find_package(ZLIB REQUIRED)
target_link_libraries(rb3pc PRIVATE ZLIB::ZLIB)

# Ship the SDK's shared libraries (runtime, Xenos GPU plugin, Tracy) next to
# the executable. The GPU plugin is only searched for there.
set_target_properties(rb3pc PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
if(NOT WIN32)
    get_target_property(_rex_runtime_lib rex::runtime IMPORTED_LOCATION_RELEASE)
    get_filename_component(_rex_lib_dir "${_rex_runtime_lib}" DIRECTORY)
    file(GLOB _rex_shared_libs "${_rex_lib_dir}/*.so")
    # Ship the compiler's libstdc++ too, so the binary still runs on systems
    # with an older libstdc++ than the one it was made with.
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.so.6
                    OUTPUT_VARIABLE _libstdcxx OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(REAL_PATH "${_libstdcxx}" _libstdcxx)
    add_custom_command(TARGET rb3pc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_rex_shared_libs} $<TARGET_FILE_DIR:rb3pc>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_libstdcxx} $<TARGET_FILE_DIR:rb3pc>/libstdc++.so.6
        VERBATIM)
endif()

# Debugging aid: build selected generated files at -O0 with full debug info so
# gdb can show guest registers (ctx.rN) at a crash. Example:
#   -DRB3PC_DEBUG_SOURCES="generated/default/rb3pc_recomp.124.cpp"
set(RB3PC_DEBUG_SOURCES "" CACHE STRING "Generated sources to build at -O0 -g")
if(RB3PC_DEBUG_SOURCES)
    set_source_files_properties(${RB3PC_DEBUG_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-O0;-g" SKIP_PRECOMPILE_HEADERS ON)
endif()
