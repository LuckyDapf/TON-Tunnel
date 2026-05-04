#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/err.h"

class WintunLwipBridge {
public:
    using LogCallback = std::function<void(const std::string& msg)>;

    WintunLwipBridge();
    ~WintunLwipBridge();

    bool start(const std::string& socksHost, uint16_t socksPort, LogCallback logger, std::string& error);
    void stop();

    void handlePacket(const uint8_t* data, size_t len);
    bool writeTunPacket(const uint8_t* data, size_t len);

    bool isRunning() const { return running_.load(); }
    const std::string& adapterName() const { return adapterName_; }
    uint32_t tunIp4() const { return tunIp4_; }

private:
    using WintunCreateAdapter_fn        = void* (*)(const wchar_t*, const wchar_t*, const GUID*);
    using WintunCloseAdapter_fn         = void  (*)(void*);
    using WintunStartSession_fn         = void* (*)(void*, DWORD);
    using WintunEndSession_fn           = void  (*)(void*);
    using WintunGetReadWaitEvent_fn     = HANDLE(*)(void*);
    using WintunReceivePacket_fn        = const uint8_t* (*)(void*, DWORD*);
    using WintunReleaseReceivePacket_fn = void  (*)(void*, const uint8_t*);
    using WintunAllocateSendPacket_fn   = uint8_t* (*)(void*, DWORD);
    using WintunSendPacket_fn           = void  (*)(void*, const uint8_t*);
    using WintunGetAdapterLUID_fn       = void  (*)(void*, void*);

    struct TcpConnState {
        struct tcp_pcb* pcb = nullptr;
        uint32_t remoteAddr = 0;
        uint16_t remotePort = 0;
        SOCKET socksSocket = INVALID_SOCKET;
        bool socksConnected = false;
        std::deque<std::vector<uint8_t>> pendingFromSocks;
        size_t pendingFromSocksSize = 0;
        bool remoteClosed = false;
        bool localClosed = false;
        static constexpr size_t kMaxPendingSize = 256 * 1024;
    };

    bool loadWintunDll(const std::string& dllPath, std::string& error);
    void unloadWintunDll();
    bool findAdapterIndex(std::string& error);
    bool initLwipStack(std::string& error);
    void shutdownLwipStack();

    static err_t netifInitCallback(struct netif* netif);
    static err_t netifLinkOutputCallback(struct netif* netif, struct pbuf* p);
    static err_t tcpAcceptCallback(void* arg, struct tcp_pcb* newpcb, err_t err);
    static err_t tcpRecvCallback(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err);
    static err_t tcpSentCallback(void* arg, struct tcp_pcb* pcb, uint16_t len);
    static void tcpErrCallback(void* arg, err_t err);
    static err_t tcpPollCallback(void* arg, struct tcp_pcb* pcb);

    bool socks5Greet(SOCKET sock);
    bool socks5Connect(SOCKET sock, uint32_t ip, uint16_t port);
    TcpConnState* createConnection(struct tcp_pcb* pcb);
    void destroyConnection(TcpConnState* state);
    void socksRelayThread(TcpConnState* state);
    void readThread();

    void handleDnsQuery(const uint8_t* data, size_t len, uint32_t srcIp, uint16_t srcPort, uint32_t dstIp, uint16_t dstPort);
    void sendDnsReply(uint32_t srcIp, uint16_t srcPort, uint16_t dnsId, const std::vector<uint8_t>& queryData, const std::string& resolvedIp);
    void feedPacketToLwip(const uint8_t* data, size_t len);

    static uint16_t ipCsum(const uint8_t* data, size_t len);
    static std::string ipToStr(uint32_t hostOrderIp);

    void logMsg(const std::string& msg) { if (logger_) logger_(msg); }

    std::atomic<bool> running_{false};
    HMODULE wintunDll_ = nullptr;
    void* adapter_ = nullptr;
    void* session_ = nullptr;
    std::string adapterName_;
    DWORD ifIndex_ = 0;
    uint32_t tunIp4_ = 0;

    WintunCreateAdapter_fn WintunCreateAdapter_ = nullptr;
    WintunCloseAdapter_fn WintunCloseAdapter_ = nullptr;
    WintunStartSession_fn WintunStartSession_ = nullptr;
    WintunEndSession_fn WintunEndSession_ = nullptr;
    WintunGetReadWaitEvent_fn WintunGetReadWaitEvent_ = nullptr;
    WintunReceivePacket_fn WintunReceivePacket_ = nullptr;
    WintunReleaseReceivePacket_fn WintunReleaseReceivePacket_ = nullptr;
    WintunAllocateSendPacket_fn WintunAllocateSendPacket_ = nullptr;
    WintunSendPacket_fn WintunSendPacket_ = nullptr;
    WintunGetAdapterLUID_fn WintunGetAdapterLUID_ = nullptr;

    struct netif* lwipNetif_ = nullptr;
    bool lwipInitialized_ = false;

    std::string socksHost_;
    uint16_t socksPort_ = 0;

    std::thread readThread_;
    mutable std::mutex tunMutex_;
    mutable std::mutex connMutex_;
    std::map<struct tcp_pcb*, std::unique_ptr<TcpConnState>> connections_;
    std::vector<std::thread> relayThreads_;
    LogCallback logger_;
};

#pragma pack(push, 1)
struct Ipv4HeaderPacked {
    uint8_t version_ihl, dscp_ecn;
    uint16_t total_length, identification, flags_fragment;
    uint8_t ttl, protocol;
    uint16_t header_checksum;
    uint32_t src_addr, dst_addr;
    uint8_t headerLength() const { return (version_ihl & 0x0F) * 4; }
    uint32_t srcAddr() const { return ntohl(src_addr); }
    uint32_t dstAddr() const { return ntohl(dst_addr); }
};
struct TcpHeaderPacked {
    uint16_t src_port, dst_port;
    uint32_t seq_num, ack_num;
    uint16_t flags_offset, window_size, checksum, urgent_pointer;
    uint8_t dataOffset() const { return (flags_offset >> 12) * 4; }
    uint16_t srcPort() const { return ntohs(src_port); }
    uint16_t dstPort() const { return ntohs(dst_port); }
    bool isSYN() const { return (flags_offset & 0x02) != 0; }
    bool isFIN() const { return (flags_offset & 0x01) != 0; }
    bool isRST() const { return (flags_offset & 0x04) != 0; }
};
struct UdpHeaderPacked {
    uint16_t src_port, dst_port, length, checksum;
    uint16_t srcPort() const { return ntohs(src_port); }
    uint16_t dstPort() const { return ntohs(dst_port); }
};
struct DnsHeader {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
};
#pragma pack(pop)
