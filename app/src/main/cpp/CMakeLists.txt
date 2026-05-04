cmake_minimum_required(VERSION 3.22.1)
project(ton_transport LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(TON_ANDROID_BUILD_ADNL_CLIENT "Build Android ADNL client backend" ON)
if (NOT TON_PREBUILT_ONLY)
    # Force Android builds onto the source backend path so stale CMake cache
    # cannot silently fall back to the deleted prebuilt runtime.
    set(TON_ANDROID_BUILD_ADNL_CLIENT ON CACHE BOOL "Build Android ADNL client backend" FORCE)
endif()
# Expected ADNL client backend prebuilt:
# app/src/main/jniLibs/<ABI>/libton_adnl_client.so
set(TON_ADNL_CLIENT_LIB_PATH "${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libton_adnl_client.so")
set(TON_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/../../../../ton-egress-node/third_party/ton")
# Use prebuilt backend only when explicitly forced.
if (TON_PREBUILT_ONLY)
    set(TON_ANDROID_BUILD_ADNL_CLIENT OFF CACHE BOOL "Build Android ADNL client backend" FORCE)
endif()
find_library(log-lib log)
find_library(dl-lib dl)

add_library(
        ton_transport
        SHARED
        ton_transport.cpp
)

set(TUN2SOCKS_CORE_ROOT "${CMAKE_SOURCE_DIR}/third_party/badvpn")

if (EXISTS "${TUN2SOCKS_CORE_ROOT}/tun2socks/tun2socks.c")
    add_library(
            tun2socks_core
            STATIC
            ${TUN2SOCKS_CORE_ROOT}/base/BLog_syslog.c
            ${TUN2SOCKS_CORE_ROOT}/system/BReactor_badvpn.c
            ${TUN2SOCKS_CORE_ROOT}/system/BSignal.c
            ${TUN2SOCKS_CORE_ROOT}/system/BConnection_unix.c
            ${TUN2SOCKS_CORE_ROOT}/system/BConnection_common.c
            ${TUN2SOCKS_CORE_ROOT}/system/BTime.c
            ${TUN2SOCKS_CORE_ROOT}/system/BUnixSignal.c
            ${TUN2SOCKS_CORE_ROOT}/system/BNetwork.c
            ${TUN2SOCKS_CORE_ROOT}/system/BDatagram_common.c
            ${TUN2SOCKS_CORE_ROOT}/system/BDatagram_unix.c
            ${TUN2SOCKS_CORE_ROOT}/flow/StreamRecvInterface.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketRecvInterface.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketPassInterface.c
            ${TUN2SOCKS_CORE_ROOT}/flow/StreamPassInterface.c
            ${TUN2SOCKS_CORE_ROOT}/flow/SinglePacketBuffer.c
            ${TUN2SOCKS_CORE_ROOT}/flow/BufferWriter.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketBuffer.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketStreamSender.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketPassConnector.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketProtoFlow.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketProtoEncoder.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketProtoDecoder.c
            ${TUN2SOCKS_CORE_ROOT}/flow/PacketPassFairQueue.c
            ${TUN2SOCKS_CORE_ROOT}/socksclient/BSocksClient.c
            ${TUN2SOCKS_CORE_ROOT}/tuntap/BTap.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/udp.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/memp.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/init.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/pbuf.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/tcp.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/tcp_out.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/sys.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/netif.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/def.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/mem.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/tcp_in.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/stats.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ip.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/timeouts.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/inet_chksum.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv4/icmp.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv4/ip4.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv4/ip4_addr.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv4/ip4_frag.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv6/ip6.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv6/nd6.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv6/icmp6.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv6/ip6_addr.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/core/ipv6/ip6_frag.c
            ${TUN2SOCKS_CORE_ROOT}/lwip/custom/sys.c
            ${TUN2SOCKS_CORE_ROOT}/tun2socks/tun2socks.c
            ${TUN2SOCKS_CORE_ROOT}/base/DebugObject.c
            ${TUN2SOCKS_CORE_ROOT}/base/BLog.c
            ${TUN2SOCKS_CORE_ROOT}/base/BPending.c
            ${TUN2SOCKS_CORE_ROOT}/flowextra/PacketPassInactivityMonitor.c
            ${TUN2SOCKS_CORE_ROOT}/tun2socks/SocksUdpGwClient.c
            ${TUN2SOCKS_CORE_ROOT}/udpgw_client/UdpGwClient.c
            ${TUN2SOCKS_CORE_ROOT}/socks_udp_client/SocksUdpClient.c
    )

    target_include_directories(
            tun2socks_core
            PRIVATE
            ${TUN2SOCKS_CORE_ROOT}
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/include/ipv4
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/include/ipv6
            ${TUN2SOCKS_CORE_ROOT}/lwip/src/include
            ${TUN2SOCKS_CORE_ROOT}/lwip/custom
    )

    target_compile_definitions(
            tun2socks_core
            PRIVATE
            BADVPN_THREADWORK_USE_PTHREAD
            BADVPN_LINUX
            BADVPN_BREACTOR_BADVPN
            _GNU_SOURCE
            BADVPN_LITTLE_ENDIAN
            BADVPN_THREAD_SAFE=1
            BADVPN_USE_SELFPIPE
            BADVPN_USE_POLL
            NDEBUG
            ANDROID
    )

    add_library(
            tun2socks_jni
            SHARED
            tun2socks_bridge.cpp
    )
    target_include_directories(tun2socks_jni PRIVATE ${TUN2SOCKS_CORE_ROOT})
    target_link_libraries(tun2socks_jni tun2socks_core ${log-lib})
endif()

if (TON_ANDROID_BUILD_ADNL_CLIENT)
    if (EXISTS "${TON_SOURCE_ROOT}/CMakeLists.txt")
        # Full adnl (UDP peer stack); AdnlExtClient is TCP-only (adnllite)
        set(TON_ONLY_TONLIB OFF CACHE BOOL "" FORCE)
        set(TON_ANDROID_ADNL_MINIMAL ON CACHE BOOL "" FORCE)
        set(TON_ANDROID_MINIMAL_CRYPTO ON CACHE BOOL "" FORCE)
        set(TON_DISABLE_QUIC_FOR_ANDROID ON CACHE BOOL "" FORCE)
        # Force TON's BuildOpenSSL.cmake to re-evaluate and rebuild Android OpenSSL
        # instead of reusing stale cache paths from previous configure attempts.
        unset(OPENSSL_ROOT_DIR CACHE)
        unset(OPENSSL_INCLUDE_DIR CACHE)
        unset(OPENSSL_CRYPTO_LIBRARY CACHE)
        unset(OPENSSL_SSL_LIBRARY CACHE)
        set(OPENSSL_FOUND FALSE CACHE BOOL "" FORCE)
        set(ZLIB_FOUND FALSE CACHE BOOL "" FORCE)
        unset(ZLIB_LIBRARY CACHE)
        unset(ZLIB_LIBRARIES CACHE)
        add_subdirectory("${TON_SOURCE_ROOT}" "${CMAKE_BINARY_DIR}/ton-android-build" EXCLUDE_FROM_ALL)

        add_library(
                ton_adnl_client
                SHARED
                ton_adnl_client_backend.cpp
        )
        target_include_directories(ton_adnl_client PRIVATE "${TON_SOURCE_ROOT}")
        target_link_libraries(
                ton_adnl_client
                adnl
                dht
                ${log-lib}
        )
        # Ensure APK gets the freshly built client backend instead of stale prebuilt from jniLibs.
        add_custom_command(
                TARGET ton_adnl_client
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:ton_adnl_client>
                        "${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libton_adnl_client.so"
                VERBATIM
        )
        target_link_libraries(ton_transport ton_adnl_client)
    else()
        message(FATAL_ERROR "TON source tree not found: ${TON_SOURCE_ROOT}")
    endif()
elseif (EXISTS "${TON_ADNL_CLIENT_LIB_PATH}")
    add_library(
            ton_adnl_client
            SHARED
            ton_adnl_client_stub.cpp
    )
    target_link_libraries(ton_adnl_client ${log-lib})
    # Keep ton_adnl_client as a real Gradle-visible target, but replace its output
    # with the already bundled prebuilt runtime to avoid rebuilding TON/OpenSSL.
    add_custom_command(
            TARGET ton_adnl_client
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${TON_ADNL_CLIENT_LIB_PATH}"
                    $<TARGET_FILE:ton_adnl_client>
            VERBATIM
    )
    target_link_libraries(ton_transport ton_adnl_client)
else()
    message(WARNING "ADNL client backend was not found. Building ton_transport without link-time ADNL backend: ${TON_ADNL_CLIENT_LIB_PATH}")
endif()

target_link_libraries(
        ton_transport
        ${log-lib}
        ${dl-lib}
)
