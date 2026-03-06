cmake_minimum_required(VERSION 3.16)

project(hft VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(hft_build_flags INTERFACE)

set(XDP_TUTORIAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../xdp-tutorial" CACHE PATH
    "Path to a local xdp-tutorial checkout used for libxdp/libbpf fallback")

find_path(LIBXDP_INCLUDE_DIR NAMES xdp/libxdp.h
    HINTS
        "${XDP_TUTORIAL_ROOT}/lib/install/include"
)
find_path(LIBBPF_INCLUDE_DIR NAMES bpf/libbpf.h
    HINTS
        "${XDP_TUTORIAL_ROOT}/lib/libbpf/src/build/usr/include"
        "${XDP_TUTORIAL_ROOT}/lib/install/include"
)
find_path(LIBBPF_UAPI_INCLUDE_DIR NAMES linux/bpf.h
    HINTS
        "${XDP_TUTORIAL_ROOT}/lib/libbpf/include/uapi"
)

find_library(LIBXDP_LIBRARY NAMES xdp libxdp
    HINTS
        "${XDP_TUTORIAL_ROOT}/lib/install/lib"
)
find_library(LIBBPF_LIBRARY NAMES bpf libbpf
    HINTS
        "${XDP_TUTORIAL_ROOT}/lib/install/lib"
        "${XDP_TUTORIAL_ROOT}/lib/libbpf/src"
)
find_library(ELF_LIBRARY NAMES elf)
find_package(ZLIB REQUIRED)

if(NOT LIBXDP_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find xdp/libxdp.h. Install libxdp development headers.")
endif()
if(NOT LIBBPF_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find bpf/libbpf.h. Install libbpf development headers.")
endif()
if(NOT LIBXDP_LIBRARY)
    message(FATAL_ERROR "Could not find libxdp.")
endif()
if(NOT LIBBPF_LIBRARY)
    message(FATAL_ERROR "Could not find libbpf.")
endif()
if(NOT ELF_LIBRARY)
    message(FATAL_ERROR "Could not find libelf.")
endif()

# CPU core pinning
macro(hft_pin_core VAR DESC DEFAULT)
    set(${VAR} "${DEFAULT}" CACHE STRING "${DESC}")
    if(NOT ${VAR} MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${VAR} must be a non-negative integer, got '${${VAR}}'")
    endif()
endmacro()

hft_pin_core(HFT_SERVER_FEED_CORE        "CPU core for server feed thread"              2)
hft_pin_core(HFT_SERVER_ORDER_RECV_CORE  "CPU core for server order-receiver thread"    3)
hft_pin_core(HFT_CLIENT_UDP_RECV_CORE    "CPU core for client UDP-receiver thread"      4)
hft_pin_core(HFT_CLIENT_ORDER_SEND_CORE  "CPU core for client order-sender thread"      5)
hft_pin_core(HFT_ENGINE_DISPATCH_CORE    "CPU core for engine dispatcher (main) thread" 6)
hft_pin_core(HFT_ENGINE_LANE1_CORE       "CPU core for engine lane-1 compute thread"    7)
hft_pin_core(HFT_ENGINE_RESET_CORE       "CPU core for engine reset lane-0 thread"      8)
hft_pin_core(HFT_ENGINE_RESET_LANE1_CORE "CPU core for engine reset lane-1 thread"      9)

# Include paths
target_include_directories(hft_build_flags INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/client
    ${CMAKE_CURRENT_SOURCE_DIR}/server
    ${CMAKE_CURRENT_SOURCE_DIR}/itch
    ${CMAKE_CURRENT_SOURCE_DIR}/ring_buffers
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/oms
    ${LIBXDP_INCLUDE_DIR}
    ${LIBBPF_INCLUDE_DIR}
    ${LIBBPF_UAPI_INCLUDE_DIR}
)

# Compiler flags
target_compile_options(hft_build_flags INTERFACE
    -O3
    -march=native
    -mtune=native
    -flto
    -fno-omit-frame-pointer       # keep for perf/flamegraph profiling
    -pipe
    -pthread
    -fstrict-aliasing
    -funroll-loops
    -DNDEBUG                      # disable assert() overhead
    -ffunction-sections           # enable dead-code elimination per-function
    -fdata-sections               # enable dead-code elimination per-variable
    -fno-plt                      # inline PLT stubs → removes one indirection per call
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    target_compile_options(hft_build_flags INTERFACE
        -mavx2
        -mfma
        -mbmi2
        -mpopcnt
    )
endif()

# Linker flags
target_link_options(hft_build_flags INTERFACE
    -O3
    -flto
    -pthread
    -Wl,--as-needed               # drop unused library dependencies
    -Wl,-O1                       # linker-level optimization (merges sections etc.)
    -Wl,--gc-sections             # dead-code elimination (pairs with -ffunction/data-sections)
    -Wl,-z,now                    # resolve all symbols at load time (no lazy PLT overhead)
    -Wl,-z,relro                  # mark GOT read-only after relocation
)

target_link_libraries(hft_build_flags INTERFACE
    Threads::Threads
    ${LIBXDP_LIBRARY}
    ${LIBBPF_LIBRARY}
    ${ELF_LIBRARY}
    ZLIB::ZLIB
)

# server
add_executable(server
    server/server.c
    server/feed.c
    server/order_receiver.c
)

target_compile_definitions(server PRIVATE
    HFT_DEFAULT_ITCH_FILE="${CMAKE_SOURCE_DIR}/data/01302020.NASDAQ_ITCH50"
    HFT_SERVER_FEED_CORE=${HFT_SERVER_FEED_CORE}
    HFT_SERVER_ORDER_RECV_CORE=${HFT_SERVER_ORDER_RECV_CORE}
)

target_link_libraries(server PRIVATE hft_build_flags)

# XDP BPF filter (only redirect UDP feed, pass TCP through)
find_program(CLANG clang)
set(XDP_FILTER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/afxdp/xdp_filter.bpf.c")
set(XDP_FILTER_OBJ "${CMAKE_BINARY_DIR}/xdp_filter.bpf.o")

if(CLANG)
    add_custom_command(
        OUTPUT  "${XDP_FILTER_OBJ}"
        COMMAND ${CLANG} -O2 -g -target bpf
                -I${LIBBPF_INCLUDE_DIR}
                $<$<BOOL:${LIBBPF_UAPI_INCLUDE_DIR}>:-I${LIBBPF_UAPI_INCLUDE_DIR}>
                -c "${XDP_FILTER_SRC}"
                -o "${XDP_FILTER_OBJ}"
        DEPENDS "${XDP_FILTER_SRC}"
        COMMENT "Building XDP feed filter BPF object"
    )
    add_custom_target(xdp_filter ALL DEPENDS "${XDP_FILTER_OBJ}")
else()
    message(WARNING "clang not found — cannot build XDP filter; AF_XDP will use the default catch-all program")
endif()

# client
add_executable(client
    client/client.c
    client/order_sender.c
    client/udp_receiver.c
    afxdp/afxdp_receiver.c
    itch/itch_handler.c
    parser/parser_to_engine.c
)

target_compile_definitions(client PRIVATE
    HFT_CLIENT_UDP_RECV_CORE=${HFT_CLIENT_UDP_RECV_CORE}
    HFT_CLIENT_ORDER_SEND_CORE=${HFT_CLIENT_ORDER_SEND_CORE}
    XDP_FILTER_OBJ="${XDP_FILTER_OBJ}"
)

target_link_libraries(client PRIVATE hft_build_flags)

if(CLANG)
    add_dependencies(client xdp_filter)
endif()

# engine
add_executable(engine
    engine/dispatcher.cpp
    engine/cpu/cpu_entry.cpp
    engine/strategy/imbalance_strat.cpp
    engine/risk/risk.cpp
    engine/oms/oms.cpp
)

target_compile_definitions(engine PRIVATE
    HFT_ENGINE_DISPATCH_CORE=${HFT_ENGINE_DISPATCH_CORE}
    HFT_ENGINE_LANE1_CORE=${HFT_ENGINE_LANE1_CORE}
    HFT_ENGINE_RESET_CORE=${HFT_ENGINE_RESET_CORE}
    HFT_ENGINE_RESET_LANE1_CORE=${HFT_ENGINE_RESET_LANE1_CORE}
)

target_link_libraries(engine PRIVATE hft_build_flags rt)

# Output directory
set_target_properties(server client engine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
