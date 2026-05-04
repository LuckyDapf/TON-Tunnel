/**
 * @file lwipopts.h
 *
 * lwIP configuration for WintunLwipBridge.
 * This file MUST be found before third_party/badvpn/lwip/custom/lwipopts.h
 * in the include path. The CMakeLists.txt adds ${CMAKE_CURRENT_SOURCE_DIR}
 * (the TON-Tunnel directory) to the include path first, which satisfies this.
 *
 * Key differences from the badvpn lwipopts.h:
 *   - LWIP_UDP=0 (UDP/DNS is handled locally, not by lwIP)
 *   - LWIP_IPV6=0 (IPv6 not needed for SOCKS5 bridge)
 *   - LWIP_ICMP=1 (respond to ICMP echo for basic connectivity)
 */

#ifndef LWIP_CUSTOM_LWIPOPTS_H
#define LWIP_CUSTOM_LWIPOPTS_H

// ─── System mode ──────────────────────────────────────────────────────────
// NO_SYS=1: no OS thread, raw/callback API only.
// lwIP runs synchronously in our thread context.
#define NO_SYS                      1
#define LWIP_TIMERS                 0
#define MEM_ALIGNMENT               4

// ─── Protocol enables ─────────────────────────────────────────────────────
#define LWIP_ARP                    0
#define ARP_QUEUEING                0
#define IP_FORWARD                  0
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_SNMP                   0
#define LWIP_IGMP                   0
#define LWIP_DNS                    0
#define LWIP_UDP                    0       // DNS handled locally, not via lwIP
#define LWIP_UDPLITE                0
#define LWIP_TCP                    1
#define LWIP_CALLBACK_API           1
#define LWIP_NETIF_API              0
#define LWIP_NETIF_LOOPBACK         0
#define LWIP_HAVE_LOOPIF            0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define PPP_SUPPORT                 0
#define LWIP_IPV6                   0       // IPv4-only for SOCKS5 bridge
#define LWIP_IPV6_MLD               0
#define LWIP_IPV6_AUTOCONFIG        0
#define LWIP_IPV6_FORWARD           0

// ─── TCP configuration ────────────────────────────────────────────────────
// We bridge to real TCP via SOCKS5, so lwIP buffers don't need to be huge.
// The real TCP connection handles retransmission and flow control.
#define MEMP_NUM_TCP_PCB_LISTEN     16
#define MEMP_NUM_TCP_PCB            1024
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (16 * TCP_MSS)  // ~23KB send buffer
#define TCP_WND                     (16 * TCP_MSS)  // ~23KB receive window
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_LISTEN_BACKLOG          16
#define LWIP_TCP_SACK_OUT           0

// ─── Memory ───────────────────────────────────────────────────────────────
// Use C library malloc/free for simplicity
#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             1

// ─── Misc ─────────────────────────────────────────────────────────────────
#define LWIP_PERF                   0
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS

// Fix IP fragment reassembly on 64-bit
#if !defined IPV6_FRAG_COPYHEADER
#define IPV6_FRAG_COPYHEADER        1
#endif

// ─── Debug (uncomment for troubleshooting) ────────────────────────────────
/*
#define LWIP_DEBUG                  1
#define IP_DEBUG                    LWIP_DBG_ON
#define NETIF_DEBUG                 LWIP_DBG_ON
#define TCP_DEBUG                   LWIP_DBG_ON
#define TCP_INPUT_DEBUG             LWIP_DBG_ON
#define TCP_OUTPUT_DEBUG            LWIP_DBG_ON
*/

#endif // LWIP_CUSTOM_LWIPOPTS_H
