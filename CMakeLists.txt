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

# Rock Band 3 Deluxe's executable, built as rb3pc-dx when it's in game/rb3dx
# (src/exe.h, src/deluxe.h). Same sources with RB3PC_DX defined, plus its
# own recompiled code: codegen runs from config/dx/rb3pc_dx_manifest.toml
# into generated/dx, as the retail build's does (generated/rexglue.cmake).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game/rb3dx/default.xex")
    set(RB3PC_DX_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/dx")
    set(RB3PC_DX_GENERATED_SOURCES "")
    if(EXISTS "${RB3PC_DX_GENERATED_DIR}/sources.cmake")
        set(_retail_generated_sources ${GENERATED_SOURCES})
        include(generated/dx/sources.cmake)
        set(RB3PC_DX_GENERATED_SOURCES ${GENERATED_SOURCES})
        set(GENERATED_SOURCES ${_retail_generated_sources})
        set_source_files_properties(${RB3PC_DX_GENERATED_SOURCES}
            PROPERTIES COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}")
    endif()
    # Every known DX executable runs on the one rb3pc-dx (they're the same TU5
    # build with a few instructions patched): tools/dx_variants.py compares
    # game/rb3dx/versions/*.xex with game/rb3dx/default.xex and writes the
    # instruction variants codegen emits switches for, plus page hashes
    # rb3pc-dx checks the installed executable against at boot.
    file(GLOB RB3PC_DX_VERSIONS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/game/rb3dx/versions/*.xex")
    add_executable(xexdump tools/xexdump/xexdump.cpp)
    target_link_libraries(xexdump PRIVATE rex::runtime)
    set_target_properties(xexdump PROPERTIES BUILD_RPATH "$ORIGIN")
    set(_dx_dumps "${CMAKE_BINARY_DIR}/dx_dumps")
    set(_dx_dump_commands COMMAND $<TARGET_FILE:xexdump>
        "${CMAKE_CURRENT_SOURCE_DIR}/game/rb3dx" default.xex "${_dx_dumps}/base")
    set(_dx_dump_dirs "")
    foreach(_xex ${RB3PC_DX_VERSIONS})
        get_filename_component(_name "${_xex}" NAME_WE)
        list(APPEND _dx_dump_commands COMMAND $<TARGET_FILE:xexdump>
            "${CMAKE_CURRENT_SOURCE_DIR}/game/rb3dx/versions" "${_name}.xex" "${_dx_dumps}/${_name}")
        list(APPEND _dx_dump_dirs "${_dx_dumps}/${_name}")
    endforeach()
    set(RB3PC_DX_VARIANTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/dx_variants.toml")
    set(RB3PC_DX_REFERENCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/dx_reference.h")
    add_custom_command(
        OUTPUT "${RB3PC_DX_VARIANTS}" "${RB3PC_DX_REFERENCE}"
        ${_dx_dump_commands}
        # The code cave and its branch are handled by a hook (config/dx/hooks.toml).
        # -I and no PYTHONHOME/PYTHONPATH: a shell started from an AppImage
        # can leak its own Python's into the build.
        COMMAND ${CMAKE_COMMAND} -E env --unset=PYTHONHOME --unset=PYTHONPATH
                python3 -I tools/dx_variants.py "${_dx_dumps}/base" "${RB3PC_DX_VARIANTS}"
                "${RB3PC_DX_REFERENCE}" --exclude 0x8275D6E0 --exclude 0x82516324-0x82516338
                ${_dx_dump_dirs}
        DEPENDS xexdump tools/dx_variants.py "${CMAKE_CURRENT_SOURCE_DIR}/game/rb3dx/default.xex"
                ${RB3PC_DX_VERSIONS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Comparing the known Rock Band 3 Deluxe executables"
        VERBATIM)

    add_custom_command(
        OUTPUT "${RB3PC_DX_GENERATED_DIR}/codegen.build.stamp" ${RB3PC_DX_GENERATED_SOURCES}
        COMMAND $<TARGET_FILE:rex::rexglue> codegen
                ${CMAKE_CURRENT_SOURCE_DIR}/config/dx/rb3pc_dx_manifest.toml
        DEPENDS "${RB3PC_DX_VARIANTS}"
        DEPFILE "${RB3PC_DX_GENERATED_DIR}/codegen.d"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating recompiled code for rb3pc-dx"
        VERBATIM)
    add_custom_target(rb3pc_dx_codegen
        DEPENDS "${RB3PC_DX_GENERATED_DIR}/codegen.build.stamp")

    add_executable(rb3pc-dx ${RB3PC_SOURCES} src/deluxe_exe.cpp "${RB3PC_DX_REFERENCE}")
    target_compile_definitions(rb3pc-dx PRIVATE RB3PC_DX)
    if(RB3PC_DX_GENERATED_SOURCES)
        add_library(rb3pc-dx_recomp OBJECT ${RB3PC_DX_GENERATED_SOURCES})
        target_include_directories(rb3pc-dx_recomp PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src ${RB3PC_DX_GENERATED_DIR})
        target_link_libraries(rb3pc-dx_recomp PRIVATE rex::runtime)
        rexglue_apply_target_settings(rb3pc-dx_recomp)
        rexglue_apply_recomp_settings(rb3pc-dx_recomp "${RB3PC_DX_GENERATED_DIR}/rb3pc_pch.h")
        add_dependencies(rb3pc-dx_recomp rb3pc_dx_codegen)
        target_link_libraries(rb3pc-dx PRIVATE rb3pc-dx_recomp)
    endif()
    add_dependencies(rb3pc-dx rb3pc_dx_codegen)
    target_include_directories(rb3pc-dx PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src ${RB3PC_DX_GENERATED_DIR})
    target_link_libraries(rb3pc-dx PRIVATE rex::runtime ZLIB::ZLIB)
    rexglue_configure_target(rb3pc-dx)
    set_target_properties(rb3pc-dx PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()

# Debugging aid: build selected generated files at -O0 with full debug info so
# gdb can show guest registers (ctx.rN) at a crash. Example:
#   -DRB3PC_DEBUG_SOURCES="generated/default/rb3pc_recomp.124.cpp"
set(RB3PC_DEBUG_SOURCES "" CACHE STRING "Generated sources to build at -O0 -g")
if(RB3PC_DEBUG_SOURCES)
    set_source_files_properties(${RB3PC_DEBUG_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-O0;-g" SKIP_PRECOMPILE_HEADERS ON)
endif()
