cmake_minimum_required(VERSION 3.22)
project(ton_tunnel_windows LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TON_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/../ton-egress-node/third_party/ton")
if (NOT EXISTS "${TON_SOURCE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "TON source tree not found: ${TON_SOURCE_ROOT}")
endif()

# Match Android/server integration choices as close as possible.
set(TON_DISABLE_QUIC_FOR_EGRESS ON CACHE BOOL "" FORCE)
set(USE_QUIC OFF CACHE BOOL "" FORCE)
set(TON_DISABLE_LIBBACKTRACE_FOR_EGRESS ON CACHE BOOL "" FORCE)
set(TON_PRINT_BACKTRACE_ON_CRASH OFF CACHE BOOL "" FORCE)
set(TDUTILS_USE_LIBBACKTRACE OFF CACHE BOOL "" FORCE)
set(PKG_CONFIG_EXECUTABLE "C:/Strawberry/perl/bin/pkg-config.bat" CACHE FILEPATH "" FORCE)
set(TON_ONLY_TONLIB OFF CACHE BOOL "" FORCE)
set(TON_ANDROID_ADNL_MINIMAL ON CACHE BOOL "" FORCE)
set(TON_ANDROID_MINIMAL_CRYPTO ON CACHE BOOL "" FORCE)
set(TON_DISABLE_QUIC_FOR_ANDROID ON CACHE BOOL "" FORCE)

if(MSVC)
    add_compile_options(/bigobj)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj")
endif()

set(PERL_EXECUTABLE "C:/Strawberry/perl/bin/perl.exe" CACHE FILEPATH "" FORCE)

add_subdirectory("${TON_SOURCE_ROOT}" "${CMAKE_BINARY_DIR}/ton-build" EXCLUDE_FROM_ALL)

# BadVPN tun2socks for Windows TUN
set(BUILD_NOTHING_BY_DEFAULT ON CACHE BOOL "" FORCE)
set(BUILD_TUN2SOCKS ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/badvpn "${CMAKE_BINARY_DIR}/badvpn-build" EXCLUDE_FROM_ALL)

add_library(ton_adnl_client_backend SHARED
    ../app/src/main/cpp/ton_adnl_client_backend.cpp
)

target_include_directories(ton_adnl_client_backend PRIVATE
    "${TON_SOURCE_ROOT}"
)

target_link_libraries(ton_adnl_client_backend PRIVATE
    adnl
    dht
)

target_compile_definitions(ton_adnl_client_backend PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
)

# lwIP source files (from badvpn)
set(LWIP_DIR third_party/badvpn/lwip)
set(LWIP_SOURCES
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/core/ipv6/ip6.c
    ${LWIP_DIR}/src/core/ipv6/nd6.c
    ${LWIP_DIR}/src/core/ipv6/icmp6.c
    ${LWIP_DIR}/src/core/ipv6/ip6_addr.c
    ${LWIP_DIR}/src/core/ipv6/ip6_frag.c
    ${LWIP_DIR}/custom/sys.c
)

add_library(lwip_core STATIC ${LWIP_SOURCES})
target_include_directories(lwip_core PUBLIC
    ${LWIP_DIR}/src/include
    ${LWIP_DIR}/src/include/ipv4
    ${LWIP_DIR}/src/include/ipv6
    ${LWIP_DIR}/custom
    ${CMAKE_CURRENT_SOURCE_DIR}  # for lwipopts.h
    third_party/badvpn  # for arch/cc.h includes (misc/*, base/*)
)
target_compile_definitions(lwip_core PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    BADVPN_USE_WINAPI
    BADVPN_LITTLE_ENDIAN
    BADVPN_BREACTOR_BADVPN
    BADVPN_THREAD_SAFE=0
    BADVPN_THREAD_SAFE=0
)

add_executable(TON_Tunnel WIN32
    TON-Tunnel.cpp
    LocalSocks5Server.cpp
    TonClientCore.cpp
    TonProtocol.cpp
    WintunLwipBridge.cpp
)

# Link lwIP into TON_Tunnel
target_link_libraries(TON_Tunnel PRIVATE
    lwip_core
)

target_include_directories(TON_Tunnel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${TON_SOURCE_ROOT}" # For TON SDK headers
    ${LWIP_DIR}/src/include
    ${LWIP_DIR}/src/include/ipv4
    ${LWIP_DIR}/src/include/ipv6
    ${LWIP_DIR}/custom
    third_party/badvpn  # for arch/cc.h includes
)

target_compile_definitions(TON_Tunnel PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    BADVPN_USE_WINAPI
    BADVPN_LITTLE_ENDIAN
    BADVPN_BREACTOR_BADVPN
    BADVPN_THREAD_SAFE=0
)

set_target_properties(TON_Tunnel PROPERTIES LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

target_link_libraries(TON_Tunnel PRIVATE
    ton_adnl_client_backend
    adnl
    dht
    # Windows system libraries for GUI, networking, etc.
    user32
    gdi32
    shell32
    ws2_32
    iphlpapi
    wininet
    dbghelp
    advapi32
    psapi
)

# Deploy Wintun runtime DLL next to TON_Tunnel.exe when available.
set(WINTUN_DLL_CANDIDATES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wintun-dist/wintun/bin/amd64/wintun.dll"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wintun.dll"
)
set(WINTUN_DLL_PATH "")
foreach(_cand IN LISTS WINTUN_DLL_CANDIDATES)
    if (EXISTS "${_cand}")
        set(WINTUN_DLL_PATH "${_cand}")
        break()
    endif()
endforeach()
if (WINTUN_DLL_PATH)
    add_custom_command(
        TARGET TON_Tunnel
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WINTUN_DLL_PATH}"
                "$<TARGET_FILE_DIR:TON_Tunnel>/wintun.dll"
        VERBATIM
    )
else()
    message(WARNING "wintun.dll was not found. Put it into third_party/wintun-dist/wintun/bin/amd64 or third_party/wintun.dll")
endif()
