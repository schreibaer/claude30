# Claude30, the native SE/30 client. 68k build with Retro68:
#   cmake -S client -B build-68k \
#     -DCMAKE_TOOLCHAIN_FILE=<Retro68-build>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build build-68k
# Produces Claude30.APPL, Claude30.bin (MacBinary) and Claude30.dsk (HFS image).
# The portable core's host tests live in tests/ and build with the host compiler.
cmake_minimum_required(VERSION 3.10)
project(Claude30 C)

set(CMAKE_C_STANDARD 99)

add_application(Claude30
    src/core/linebuf.c
    src/core/prompt.c
    src/core/protocol.c
    src/core/batch.c
    src/core/mdstyle.c
    src/core/files.c
    src/core/crc32.c
    src/core/mascot.c
    src/core/heartbeat.c
    src/core/promptmarks.c
    src/core/agentline.c
    src/core/permsum.c
    src/core/fold.c
    src/core/chatlabel.c
    src/core/usage.c
    src/core/rules.c
    src/mac/main.c
    src/mac/serial.c
    src/mac/modem.c
    src/mac/window.c
    src/mac/permdlg.c
    src/mac/prefs.c
    src/mac/log.c
    src/mac/linktest.c
    src/mac/tabs.c
    src/mac/picker.c
    src/mac/modeldlg.c
    src/mac/opendlg.c
    src/mac/filewin.c
    claude30.r
    CREATOR "Cl30"
    TYPE "APPL")

target_include_directories(Claude30 PRIVATE src/core src/mac)
target_compile_options(Claude30 PRIVATE -Wall -Wno-pointer-sign -ffunction-sections -O2)
set_target_properties(Claude30 PROPERTIES LINK_FLAGS "-Wl,-gc-sections")

# Retro68 leaves the Finder's "has bundle" flag clear, so the Finder would
# ignore the app's icon. Set it in the MacBinary file, then rebuild the disk
# image from that file with Retro68's hfsutils.
find_package(Python3 COMPONENTS Interpreter)
get_filename_component(RETRO68_BIN "${REZ}" DIRECTORY)
if(Python3_Interpreter_FOUND AND EXISTS "${RETRO68_BIN}/hcopy")
    add_custom_command(TARGET Claude30_APPL POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/set_bundle_bit.py Claude30.bin
        COMMAND ${RETRO68_BIN}/hmount Claude30.dsk
        COMMAND ${RETRO68_BIN}/hdel :Claude30
        COMMAND ${RETRO68_BIN}/hcopy -m Claude30.bin :
        COMMAND ${RETRO68_BIN}/humount
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Setting the bundle bit in Claude30.bin and Claude30.dsk")
endif()
