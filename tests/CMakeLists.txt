# Portable unit tests + fuzz harness for the protocol parsers.
#
# Builds ONLY the platform-independent parsing code (protocol/RDPDRParse.c), so
# it works on Linux/macOS without FreeRDP or Cocoa. Two ways to use it:
#
#   # standalone (what CI does on ubuntu):
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
#
#   # from the full macOS daemon build:
#   cmake -S . -B build -DRDP_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
#
# Fuzzing (needs clang; libFuzzer + ASan + UBSan):
#   CC=clang CXX=clang++ cmake -S tests -B build-fuzz -DRDP_FUZZ=ON
#   cmake --build build-fuzz && ./build-fuzz/fuzz_pdu -max_total_time=30
cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(macos-rdp-protocol-tests LANGUAGES C CXX)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

get_filename_component(RDP_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

option(RDP_FUZZ "Build the libFuzzer harness (requires clang)" OFF)

set(RDP_PARSE_WARNINGS -Wall -Wextra -Wconversion -Wno-sign-conversion -Wshadow)

# ── Portable parser library ───────────────────────────────────────────────
add_library(rdp_protocol_parse STATIC
    ${RDP_REPO_ROOT}/protocol/RDPDRParse.c
)
target_include_directories(rdp_protocol_parse PUBLIC ${RDP_REPO_ROOT})
target_compile_options(rdp_protocol_parse PRIVATE ${RDP_PARSE_WARNINGS})

# ── Unit tests ────────────────────────────────────────────────────────────
add_executable(test_rdpdr_parse test_rdpdr_parse.c)
target_link_libraries(test_rdpdr_parse PRIVATE rdp_protocol_parse)
target_compile_options(test_rdpdr_parse PRIVATE ${RDP_PARSE_WARNINGS})
add_test(NAME rdpdr_parse COMMAND test_rdpdr_parse)

# ── Fuzz harness ──────────────────────────────────────────────────────────
if(RDP_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "RDP_FUZZ=ON requires clang/clang++ (libFuzzer); "
                            "got ${CMAKE_C_COMPILER_ID}/${CMAKE_CXX_COMPILER_ID}")
    endif()
    set(RDP_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1)
    # The library under fuzz must be instrumented too.
    target_compile_options(rdp_protocol_parse PRIVATE ${RDP_FUZZ_FLAGS})
    add_executable(fuzz_pdu fuzz/fuzz_pdu.cc)
    target_link_libraries(fuzz_pdu PRIVATE rdp_protocol_parse)
    target_compile_options(fuzz_pdu PRIVATE ${RDP_FUZZ_FLAGS})
    target_link_options(fuzz_pdu PRIVATE ${RDP_FUZZ_FLAGS})
    # The unit test picks up the instrumented library; it needs the runtime too
    # (but not the fuzzer main), so link with the sanitizers only.
    target_link_options(test_rdpdr_parse PRIVATE -fsanitize=address,undefined)
endif()
