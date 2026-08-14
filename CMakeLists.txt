cmake_minimum_required(VERSION 3.22)
project(udp_multicast_receiver VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -march=native -g -DNDEBUG")

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

option(ASAN "Enable AddressSanitizer" OFF)
if(ASAN)
    add_compile_options(-fsanitize=address -g -O1)
    add_link_options(-fsanitize=address)
endif()

option(TSAN "Enable ThreadSanitizer" OFF)
if(TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)
find_library(PCAP_LIB pcap)

# ── Header-only core library ──────────────────────────────────────────────────

add_library(feed_iface INTERFACE)
target_include_directories(feed_iface INTERFACE include)
target_link_libraries(feed_iface INTERFACE Threads::Threads)

# ── Optional binaries (require libpcap) ──────────────────────────────────────

if(PCAP_LIB)
    message(STATUS "libpcap found: ${PCAP_LIB}")
    target_link_libraries(feed_iface INTERFACE ${PCAP_LIB})

    add_executable(receiver src/main.cpp)
    target_link_libraries(receiver PRIVATE feed_iface)

    add_executable(gen_pcap tools/gen_pcap.cpp)
    target_link_libraries(gen_pcap PRIVATE feed_iface)
else()
    message(STATUS "libpcap not found — skipping receiver and gen_pcap binaries")
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

add_executable(test_wire_format tests/test_wire_format.cpp)
target_link_libraries(test_wire_format PRIVATE feed_iface)
add_test(NAME WireFormat COMMAND test_wire_format)

add_executable(test_gap_buffer tests/test_gap_buffer.cpp)
target_link_libraries(test_gap_buffer PRIVATE feed_iface)
add_test(NAME GapBuffer COMMAND test_gap_buffer)

add_executable(test_pcap_replay tests/test_pcap_replay.cpp)
target_link_libraries(test_pcap_replay PRIVATE feed_iface)
add_test(NAME PcapReplay COMMAND test_pcap_replay)
