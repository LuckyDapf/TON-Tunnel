cmake_minimum_required(VERSION 3.16)
project(ton_egress_node LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(TON_EGRESS_BUILD_ADNL_BACKEND "Build real ADNL backend shared library" OFF)
option(TON_DISABLE_QUIC_FOR_EGRESS "Disable QUIC/ngtcp2 for VPS egress build" ON)
option(TON_DISABLE_LIBBACKTRACE_FOR_EGRESS "Disable libbacktrace for VPS egress build" ON)

add_executable(ton_egress_node
    src/main.cpp
    src/config.cpp
    src/protocol.cpp
    src/egress_node.cpp
    src/ton_transport_stub.cpp
    src/ton_transport_factory.cpp
)

target_include_directories(ton_egress_node PRIVATE include)

if(UNIX AND NOT APPLE)
    target_link_libraries(ton_egress_node PRIVATE dl)
endif()

if(TON_EGRESS_BUILD_ADNL_BACKEND)
    if(TON_DISABLE_QUIC_FOR_EGRESS)
        set(TON_DISABLE_QUIC_FOR_EGRESS ON CACHE BOOL "" FORCE)
        set(USE_QUIC OFF CACHE BOOL "" FORCE)
        message(STATUS "QUIC/ngtcp2 disabled for VPS egress build")
    endif()
    if(TON_DISABLE_LIBBACKTRACE_FOR_EGRESS)
        set(TON_DISABLE_LIBBACKTRACE_FOR_EGRESS ON CACHE BOOL "" FORCE)
        set(TON_PRINT_BACKTRACE_ON_CRASH OFF CACHE BOOL "" FORCE)
        set(TDUTILS_USE_LIBBACKTRACE OFF CACHE BOOL "" FORCE)
        message(STATUS "libbacktrace disabled for VPS egress build")
    endif()
    set(TON_ONLY_TONLIB OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/ton third_party/ton-build EXCLUDE_FROM_ALL)

    add_library(ton_adnl_backend SHARED
        src/ton_adnl_backend.cpp
    )
    target_include_directories(ton_adnl_backend PRIVATE
        include
        third_party/ton
    )
    target_link_libraries(ton_adnl_backend PRIVATE
        adnl
        dht
        rldp
        tdactor
        tdutils
        tdnet
        ton_crypto
        tl_api
        keys
        keyring
    )
endif()
