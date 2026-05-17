# BlueSCSI DaynaPORT sleep/wake handler
#
# Build (68k, from the repo root):
#
#   mkdir build && cd build
#   cmake .. \
#     -DCMAKE_TOOLCHAIN_FILE=~/Git/Retro68-build/toolchain/\
#         m68k-apple-macos/cmake/retro68.toolchain.cmake
#   make
#
# Produces (in build/):
#
#   BlueSCSISleepINIT.bin       — production INIT (silent on success)
#   BlueSCSISleepINIT-diag.bin  — diagnostic INIT (verbose log + alerts)
#
# Both binaries are built from init.c; the diagnostic variant is
# compiled with -DBLUESCSI_SLEEP_DIAGNOSTIC=1.

cmake_minimum_required(VERSION 3.10)
project(BlueSCSISleep C)

set(CMAKE_C_STANDARD 90)

if(REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# ── Helper: build an INIT variant from a source + rez file ──────────────────
#
# add_bluescsi_init_variant(<target> <output-stem> <source-c> <rez-file>
#                           [DEFINES <macro=value>...])
#
# Produces <output-stem>.flt, <output-stem>.bin, <output-stem>.dsk.
function(add_bluescsi_init_variant target output_stem source_c rez_file)
    cmake_parse_arguments(BSI "" "" "DEFINES" ${ARGN})

    add_executable(${target} ${source_c})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${output_stem}.flt
        LINK_FLAGS  -Wl,--mac-flat)
    target_compile_options(${target} PRIVATE -O1 -Wall -Wno-main)
    if(BSI_DEFINES)
        target_compile_definitions(${target} PRIVATE ${BSI_DEFINES})
    endif()

    add_custom_command(
        OUTPUT  ${output_stem}.bin ${output_stem}.dsk
        COMMAND ${REZ} ${REZ_INCLUDE_FLAG}
                       ${CMAKE_CURRENT_SOURCE_DIR}/${rez_file}
                       -o ${output_stem}.bin
                       --cc ${output_stem}.dsk
                       --cc ${output_stem}
                       -t INIT
        DEPENDS ${target} ${rez_file})
    add_custom_target(${target}_INIT ALL DEPENDS ${output_stem}.dsk)
endfunction()

# ── Production INIT (silent on success; alerts on error) ────────────────────
add_bluescsi_init_variant(BlueSCSISleepINIT
                          BlueSCSISleepINIT
                          init.c
                          init.r)

# ── Diagnostic INIT (verbose log + alerts) ──────────────────────────────────
add_bluescsi_init_variant(BlueSCSISleepINITDiag
                          BlueSCSISleepINIT-diag
                          init.c
                          init-diag.r
                          DEFINES BLUESCSI_SLEEP_DIAGNOSTIC=1)
