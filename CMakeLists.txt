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

set(_ob_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/orderbook")
if(NOT EXISTS "${_ob_dir}/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/orderbook is missing. Run: git submodule update --init --recursive")
endif()
set(ORDERBOOK_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/orderbook EXCLUDE_FROM_ALL)

add_library(hft_build_flags INTERFACE)

set(XDP_TUTORIAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xdp-tutorial" CACHE PATH
    "Path to the bundled xdp-tutorial submodule used for libxdp/libbpf headers/libs.
     Default points at the in-tree submodule. Override only to use a system-installed
     libxdp/libbpf or an out-of-tree checkout.")

if(NOT EXISTS "${XDP_TUTORIAL_ROOT}/lib/libbpf/src/libbpf.c")
    message(WARNING
        "xdp-tutorial submodule appears uninitialized at ${XDP_TUTORIAL_ROOT}.\n"
        "  Run: git submodule update --init --recursive\n"
        "  Then build libbpf + libxdp under that tree (see README -> Build).")
endif()

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

macro(hft_pin_core VAR DESC DEFAULT)
    set(${VAR} "${DEFAULT}" CACHE STRING "${DESC}")
    if(NOT ${VAR} MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${VAR} must be a non-negative integer, got '${${VAR}}'")
    endif()
endmacro()

hft_pin_core(HFT_SERVER_FEED_CORE "CPU core for server feed thread" 2)
hft_pin_core(HFT_SERVER_ORDER_RECV_CORE "CPU core for server order-receiver thread" 3)
hft_pin_core(HFT_CLIENT_UDP_RECV_CORE "CPU core for trading RX thread" 4)
hft_pin_core(HFT_CLIENT_ORDER_SEND_CORE "CPU core for trading TX thread" 5)
hft_pin_core(HFT_ENGINE_DISPATCH_CORE "CPU core for trading dispatcher thread" 6)

target_include_directories(hft_build_flags INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBXDP_INCLUDE_DIR}
    ${LIBBPF_INCLUDE_DIR}
    ${LIBBPF_UAPI_INCLUDE_DIR}
)

target_compile_options(hft_build_flags INTERFACE
    -O3
    -march=native
    -mtune=native
    -flto
    -fomit-frame-pointer
    -pipe
    -pthread
    -fstrict-aliasing
    -funroll-loops
    -DNDEBUG
    -ffunction-sections
    -fdata-sections
    -fno-plt
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    target_compile_options(hft_build_flags INTERFACE
        -mavx2
        -mfma
        -mbmi2
        -mpopcnt
    )
endif()

target_link_options(hft_build_flags INTERFACE
    -O3
    -flto
    -pthread
    -Wl,--as-needed
    -Wl,-O1
    -Wl,--gc-sections
    -Wl,-z,now
    -Wl,-z,relro
)

target_link_libraries(hft_build_flags INTERFACE
    Threads::Threads
    ${LIBXDP_LIBRARY}
    ${LIBBPF_LIBRARY}
    ${ELF_LIBRARY}
    ZLIB::ZLIB
)

add_executable(server
    apps/server/server.c
    apps/server/feed.c
    apps/server/order_receiver.c
    apps/server/rewinder.c
)

target_compile_definitions(server PRIVATE
    HFT_DEFAULT_ITCH_FILE="${CMAKE_SOURCE_DIR}/data/01302020.NASDAQ_ITCH50"
    HFT_SERVER_FEED_CORE=${HFT_SERVER_FEED_CORE}
    HFT_SERVER_ORDER_RECV_CORE=${HFT_SERVER_ORDER_RECV_CORE}
)

target_link_libraries(server PRIVATE hft_build_flags)

find_program(CLANG clang)
set(XDP_FILTER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/afxdp/xdp_filter.bpf.c")
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
    message(WARNING "clang not found; AF_XDP will use the default catch-all program")
endif()

add_executable(trading
    apps/engine/dispatcher.cpp
    apps/client/order_sender.c
    apps/client/udp_receiver.c
    src/afxdp/afxdp_receiver.c
    src/afxdp/afxdp_tx.c
    src/telemetry/telemetry.cpp
    src/itch/itch_handler.c
    src/engine/risk/risk.cpp
    src/engine/oms/oms.cpp
)

target_compile_definitions(trading PRIVATE
    HFT_ENGINE_DISPATCH_CORE=${HFT_ENGINE_DISPATCH_CORE}
    HFT_CLIENT_UDP_RECV_CORE=${HFT_CLIENT_UDP_RECV_CORE}
    HFT_CLIENT_ORDER_SEND_CORE=${HFT_CLIENT_ORDER_SEND_CORE}
    XDP_FILTER_OBJ="${XDP_FILTER_OBJ}"
)

target_link_libraries(trading PRIVATE hft_build_flags orderbook::orderbook rt)

if(CLANG)
    add_dependencies(trading xdp_filter)
endif()

add_executable(wire_observer
    apps/observer/wire_observer.cpp
)

target_link_libraries(wire_observer PRIVATE hft_build_flags)

set_target_properties(server trading wire_observer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
