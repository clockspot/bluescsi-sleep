# BlueSCSI DaynaPORT sleep/wake handler
#
# Builds two targets:
#
#   BlueSCSISleep      — standalone application (launch manually before sleeping)
#   BlueSCSISleepINIT  — System Extension that loads at boot (preferred)
#
# Build (68k, from the repo root):
#
#   mkdir build && cd build
#   cmake .. \
#     -DCMAKE_TOOLCHAIN_FILE=~/Git/Retro68-build/toolchain/\
#         m68k-apple-macos/cmake/retro68.toolchain.cmake
#   make

cmake_minimum_required(VERSION 3.10)
project(BlueSCSISleep C)

set(CMAKE_C_STANDARD 90)

# ── Application version ───────────────────────────────────────────────────────
add_application(BlueSCSISleep
    main.c
)
target_compile_options(BlueSCSISleep PRIVATE -O1 -Wall -Wno-main)

# ── System Extension (INIT) version ──────────────────────────────────────────

# Step 1: build the flat code resource binary
add_executable(BlueSCSISleepINIT
    init.c
)
set_target_properties(BlueSCSISleepINIT PROPERTIES
    OUTPUT_NAME BlueSCSISleepINIT.flt
    LINK_FLAGS  -Wl,--mac-flat)
target_compile_options(BlueSCSISleepINIT PRIVATE -O1 -Wall -Wno-main)

if(REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# Step 2: Rez packages the .flt as an 'INIT' resource inside a Mac binary
add_custom_command(
    OUTPUT  BlueSCSISleepINIT.bin BlueSCSISleepINIT.dsk
    COMMAND ${REZ} ${REZ_INCLUDE_FLAG}
                   ${CMAKE_CURRENT_SOURCE_DIR}/init.r
                   -o BlueSCSISleepINIT.bin
                   --cc BlueSCSISleepINIT.dsk
                   --cc BlueSCSISleepINIT
                   -t INIT
    DEPENDS BlueSCSISleepINIT init.r)

add_custom_target(BlueSCSISleepINIT_INIT ALL DEPENDS BlueSCSISleepINIT.dsk)

# ── Minimal diagnostic INIT (do-nothing _start) ───────────────────────────

add_executable(BlueSCSISleepMinimalINIT
    init_minimal.c
)
set_target_properties(BlueSCSISleepMinimalINIT PROPERTIES
    OUTPUT_NAME BlueSCSISleepMinimalINIT.flt
    LINK_FLAGS  -Wl,--mac-flat)
target_compile_options(BlueSCSISleepMinimalINIT PRIVATE -O1 -Wall -Wno-main)

add_custom_command(
    OUTPUT  BlueSCSISleepMinimalINIT.bin BlueSCSISleepMinimalINIT.dsk
    COMMAND ${REZ} ${REZ_INCLUDE_FLAG}
                   ${CMAKE_CURRENT_SOURCE_DIR}/init_minimal.r
                   -o BlueSCSISleepMinimalINIT.bin
                   --cc BlueSCSISleepMinimalINIT.dsk
                   --cc BlueSCSISleepMinimalINIT
                   -t INIT
    DEPENDS BlueSCSISleepMinimalINIT init_minimal.r)

add_custom_target(BlueSCSISleepMinimalINIT_INIT ALL DEPENDS BlueSCSISleepMinimalINIT.dsk)
