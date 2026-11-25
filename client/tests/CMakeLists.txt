# Host build of the portable core and its unit tests.
#   cmake -S client/tests -B build-host && cmake --build build-host && ctest --test-dir build-host
cmake_minimum_required(VERSION 3.10)
project(claude30_core_tests C)

set(CMAKE_C_STANDARD 99)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Werror)
endif()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../src/core)
add_library(core STATIC
    ${CORE}/linebuf.c
    ${CORE}/prompt.c
    ${CORE}/protocol.c
    ${CORE}/batch.c
    ${CORE}/files.c
    ${CORE}/crc32.c
    ${CORE}/mascot.c
    ${CORE}/promptmarks.c
    ${CORE}/agentline.c)
target_include_directories(core PUBLIC ${CORE})

enable_testing()
foreach(t test_linebuf_prompt test_protocol test_batch test_files test_mascot test_promptmarks test_agentline)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} core)
    target_compile_definitions(${t} PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# End-to-end run of the core against bridge.py --demo and the stress bridge,
# over pipes. Needs python3 and a POSIX host.
find_program(PYTHON3 python3)
if(PYTHON3 AND UNIX)
    add_executable(test_bridge_session test_bridge_session.c)
    target_link_libraries(test_bridge_session core)
    target_compile_definitions(test_bridge_session PRIVATE
        BRIDGE_PY="${CMAKE_CURRENT_SOURCE_DIR}/../../bridge.py"
        STRESS_PY="${CMAKE_CURRENT_SOURCE_DIR}/../../tools/emu/stress_bridge.py")
    add_test(NAME test_bridge_session COMMAND test_bridge_session)
endif()
