// WintunLwipBridge.cpp — wintun + lwIP + SOCKS5 bridge
#include "WintunLwipBridge.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <iphlpapi.h>
#include <netioapi.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/err.h"

// ── Helpers ──────────────────────────────────────────────────────────
namespace {

bool sendExact(SOCKET s, const uint8_t* d, int n) {
    int sent = 0;
    while (sent < n) { int r = ::send(s, (const char*)(d+sent), n-sent, 0); if (r <= 0) return false; sent += r; }
    return true;
}
bool recvExact(SOCKET s, uint8_t* d, int n) {
    int got = 0;
    while (got < n) { int r = ::recv(s, (char*)(d+got), n-got, 0); if (r <= 0) return false; got += r; }
    return true;
}
void closeSock(SOCKET s) { if (s != INVALID_SOCKET) { ::shutdown(s, SD_BOTH); ::closesocket(s); } }

std::vector<std::string> resolveHost(const std::string& host) {
    std::vector<std::string> res;
    addrinfo hints{}, *ai = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &ai) == 0) {
        for (auto* p = ai; p; p = p->ai_next) {
            char buf[INET_ADDRSTRLEN];
            auto* sin = (sockaddr_in*)p->ai_addr;
            inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
            res.push_back(buf);
        }
        freeaddrinfo(ai);
    }
    return res;
}

// DNS wire helpers
int parseDnsName(const uint8_t* pkt, size_t len, size_t off, std::string& out) {
    out.clear();
    bool jumped = false; size_t jumps = 0; size_t orig = off;
    while (off < len && jumps < 20) {
        uint8_t l = pkt[off];
        if (l == 0) { if (!jumped) off++; if (!out.empty() && out.back()=='.') out.pop_back(); return (int)(off - orig); }
        if ((l & 0xC0) == 0xC0) {
            if (off+1 >= len) return -1;
            uint16_t ptr = ((l & 0x3F) << 8) | pkt[off+1];
            if (ptr >= len) return -1;
            if (!jumped) { orig = off+2; jumped = true; }
            off = ptr; jumps++; continue;
        }
        off++;
        for (uint8_t i = 0; i < l && off < len; i++) { char c = (char)pkt[off++]; out += c; }
        out += '.';
    }
    return -1;
}

std::vector<uint8_t> buildDnsName(const std::string& name) {
    std::vector<uint8_t> out;
    size_t s = 0;
    while (s < name.size()) {
        size_t dot = name.find('.', s);
        if (dot == std::string::npos) dot = name.size();
        size_t n = dot - s;
        out.push_back((uint8_t)n);
        for (size_t i = 0; i < n; i++) out.push_back((uint8_t)name[s+i]);
        s = dot + 1;
    }
    out.push_back(0);
    return out;
}
}

// ── Constructor / Destructor ──────────────────────────────────────────

WintunLwipBridge::WintunLwipBridge() { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); }
WintunLwipBridge::~WintunLwipBridge() { stop(); WSACleanup(); }

// ── start / stop ──────────────────────────────────────────────────────

bool WintunLwipBridge::start(const std::string& sh, uint16_t sp, LogCallback logger, std::string& error) {
    if (running_.load()) { error = "already running"; return false; }
    socksHost_ = sh; socksPort_ = sp; logger_ = std::move(logger);
    logMsg("[Bridge] Starting...");

    if (!loadWintunDll("wintun.dll", error)) { logMsg("[Bridge] " + error); return false; }
    adapter_ = WintunCreateAdapter_(L"TON-Tunnel", L"HOPE", nullptr);
    if (!adapter_) { error = "WintunCreateAdapter failed gle=" + std::to_string(GetLastError()); logMsg("[Bridge] " + error); unloadWintunDll(); return false; }
    adapterName_ = "TON-Tunnel";
    session_ = WintunStartSession_(adapter_, 0x400000);
    if (!session_) { error = "WintunStartSession failed"; logMsg("[Bridge] " + error); WintunCloseAdapter_(adapter_); adapter_ = nullptr; unloadWintunDll(); return false; }

    for (int i = 0; i < 20; i++) { if (findAdapterIndex(error)) break; std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
    if (ifIndex_ == 0) { tunIp4_ = (10 << 24) | (0 << 16) | (0 << 8) | 1; logMsg("[Bridge] Using default IP 10.0.0.1"); }

    if (!initLwipStack(error)) { logMsg("[Bridge] " + error); WintunEndSession_(session_); WintunCloseAdapter_(adapter_); adapter_ = nullptr; unloadWintunDll(); return false; }

    running_.store(true);
    readThread_ = std::thread(&WintunLwipBridge::readThread, this);
    logMsg("[Bridge] Started");
    return true;
}

void WintunLwipBridge::stop() {
    if (!running_.exchange(false)) return;
    if (readThread_.joinable()) readThread_.join();
    {
        std::lock_guard<std::mutex> lk(connMutex_);
        for (auto& kv : connections_) { kv.second->localClosed = true; if (kv.second->socksSocket != INVALID_SOCKET) closeSock(kv.second->socksSocket); }
        connections_.clear();
    }
    for (auto& t : relayThreads_) if (t.joinable()) t.join();
    relayThreads_.clear();
    shutdownLwipStack();
    if (session_) { WintunEndSession_(session_); session_ = nullptr; }
    if (adapter_) { WintunCloseAdapter_(adapter_); adapter_ = nullptr; }
    unloadWintunDll();
}

// ── wintun DLL ────────────────────────────────────────────────────────

bool WintunLwipBridge::loadWintunDll(const std::string& path, std::string& error) {
    std::wstring wp(path.begin(), path.end());
    wintunDll_ = LoadLibraryW(wp.c_str());
    if (!wintunDll_) {
        wchar_t exePath[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring full(exePath, exePath + n);
            const size_t slash = full.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                full = full.substr(0, slash + 1) + L"wintun.dll";
                wintunDll_ = LoadLibraryW(full.c_str());
            }
        }
    }
    if (!wintunDll_) { error = "LoadLibrary wintun.dll failed (put wintun.dll near TON_Tunnel.exe)"; return false; }
    #define LOAD(fn) fn##_ = (fn##_fn)GetProcAddress(wintunDll_, #fn); if (!fn##_) { error = "Missing " #fn; unloadWintunDll(); return false; }
    LOAD(WintunCreateAdapter); LOAD(WintunCloseAdapter); LOAD(WintunStartSession); LOAD(WintunEndSession);
    LOAD(WintunGetReadWaitEvent); LOAD(WintunReceivePacket); LOAD(WintunReleaseReceivePacket);
    LOAD(WintunAllocateSendPacket); LOAD(WintunSendPacket); LOAD(WintunGetAdapterLUID);
    #undef LOAD
    return true;
}

void WintunLwipBridge::unloadWintunDll() {
    if (wintunDll_) { FreeLibrary(wintunDll_); wintunDll_ = nullptr; }
    #define NUL(fn) fn##_ = nullptr
    NUL(WintunCreateAdapter); NUL(WintunCloseAdapter); NUL(WintunStartSession); NUL(WintunEndSession);
    NUL(WintunGetReadWaitEvent); NUL(WintunReceivePacket); NUL(WintunReleaseReceivePacket);
    NUL(WintunAllocateSendPacket); NUL(WintunSendPacket); NUL(WintunGetAdapterLUID);
    #undef NUL
}

bool WintunLwipBridge::findAdapterIndex(std::string& error) {
    ULONG len = 0;
    GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* addrs = (PIP_ADAPTER_ADDRESSES)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addrs, &len) != ERROR_SUCCESS) { error = "GetAdaptersAddresses failed"; return false; }
    for (auto* p = addrs; p; p = p->Next) {
        std::wstring wname(p->FriendlyName ? p->FriendlyName : L"");
        std::string name(wname.begin(), wname.end());
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name.find("ton-tunnel") != std::string::npos || name.find("wintun") != std::string::npos) {
            ifIndex_ = p->IfIndex;
            for (auto* u = p->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family == AF_INET) {
                    sockaddr_in* sin = (sockaddr_in*)u->Address.lpSockaddr;
                    tunIp4_ = ntohl(sin->sin_addr.s_addr);
                    break;
                }
            }
            return true;
        }
    }
    return false;
}

// ── lwIP stack ────────────────────────────────────────────────────────

bool WintunLwipBridge::initLwipStack(std::string& error) {
    lwip_init();
    lwipNetif_ = new netif{};
    memset(lwipNetif_, 0, sizeof(netif));
    lwipNetif_->state = this;
    auto* ni = netif_add(lwipNetif_, nullptr, nullptr, nullptr, lwipNetif_, netifInitCallback, ip_input);
    if (!ni) { error = "netif_add failed"; delete lwipNetif_; lwipNetif_ = nullptr; return false; }
    netif_set_default(lwipNetif_);
    netif_set_up(lwipNetif_);
    lwipInitialized_ = true;
    return true;
}

void WintunLwipBridge::shutdownLwipStack() {
    if (lwipInitialized_ && lwipNetif_) { netif_remove(lwipNetif_); delete lwipNetif_; lwipNetif_ = nullptr; lwipInitialized_ = false; }
}

// ── lwIP callbacks ────────────────────────────────────────────────────

err_t WintunLwipBridge::netifInitCallback(struct netif* ni) {
    ni->name[0] = 't'; ni->name[1] = 'n';
    ni->mtu = 1500;
    ni->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    ni->linkoutput = netifLinkOutputCallback;
    return ERR_OK;
}

err_t WintunLwipBridge::netifLinkOutputCallback(struct netif* ni, struct pbuf* p) {
    auto* self = (WintunLwipBridge*)ni->state;
    if (!self || !self->running_.load()) return ERR_IF;
    std::vector<uint8_t> buf(p->tot_len);
    pbuf_copy_partial(p, buf.data(), p->tot_len, 0);
    self->writeTunPacket(buf.data(), buf.size());
    return ERR_OK;
}

err_t WintunLwipBridge::tcpAcceptCallback(void* arg, struct tcp_pcb* newpcb, err_t err) {
    auto* self = (WintunLwipBridge*)arg;
    if (!self || err != ERR_OK || !newpcb) return ERR_VAL;
    auto* st = self->createConnection(newpcb);
    if (!st) { tcp_abort(newpcb); return ERR_ABRT; }
    return ERR_OK;
}

err_t WintunLwipBridge::tcpRecvCallback(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    auto* self = (WintunLwipBridge*)arg;
    if (!self || !pcb) return ERR_VAL;
    std::lock_guard<std::mutex> lk(self->connMutex_);
    auto it = self->connections_.find(pcb);
    if (it == self->connections_.end()) { if (p) pbuf_free(p); return ERR_OK; }
    auto* st = it->second.get();
    if (p == nullptr) {
        st->localClosed = true;
        if (st->socksSocket != INVALID_SOCKET) { ::shutdown(st->socksSocket, SD_SEND); }
    } else if (st->socksSocket != INVALID_SOCKET && st->socksConnected) {
        if (!sendExact(st->socksSocket, (uint8_t*)p->payload, p->tot_len)) { st->remoteClosed = true; closeSock(st->socksSocket); st->socksSocket = INVALID_SOCKET; }
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
    } else {
        pbuf_free(p);
    }
    return ERR_OK;
}

err_t WintunLwipBridge::tcpSentCallback(void* arg, struct tcp_pcb* pcb, uint16_t len) {
    auto* self = (WintunLwipBridge*)arg;
    std::lock_guard<std::mutex> lk(self->connMutex_);
    auto it = self->connections_.find(pcb);
    if (it == self->connections_.end()) return ERR_OK;
    auto* st = it->second.get();
    while (!st->pendingFromSocks.empty() && st->pendingFromSocksSize > 0) {
        auto& buf = st->pendingFromSocks.front();
        err_t wr = tcp_write(pcb, buf.data(), buf.size(), TCP_WRITE_FLAG_COPY);
        if (wr == ERR_OK) { st->pendingFromSocksSize -= buf.size(); st->pendingFromSocks.pop_front(); tcp_output(pcb); }
        else break;
    }
    return ERR_OK;
}

void WintunLwipBridge::tcpErrCallback(void* arg, err_t err) {
    auto* self = (WintunLwipBridge*)arg;
    std::lock_guard<std::mutex> lk(self->connMutex_);
    for (auto& kv : self->connections_) {
        if (kv.first) { tcp_arg(kv.first, nullptr); tcp_close(kv.first); }
        if (kv.second->socksSocket != INVALID_SOCKET) closeSock(kv.second->socksSocket);
    }
    self->connections_.clear();
}

err_t WintunLwipBridge::tcpPollCallback(void* arg, struct tcp_pcb* pcb) {
    auto* self = (WintunLwipBridge*)arg;
    std::lock_guard<std::mutex> lk(self->connMutex_);
    auto it = self->connections_.find(pcb);
    if (it == self->connections_.end()) return ERR_OK;
    auto* st = it->second.get();
    if (st->remoteClosed && !st->localClosed) { st->localClosed = true; tcp_close(pcb); self->connections_.erase(it); }
    return ERR_OK;
}

// ── Connections ────────────────────────────────────────────────────────

auto WintunLwipBridge::createConnection(struct tcp_pcb* pcb) -> TcpConnState* {
    auto st = std::make_unique<TcpConnState>();
    st->pcb = pcb;
    tcp_arg(pcb, this);
    tcp_recv(pcb, tcpRecvCallback);
    tcp_sent(pcb, tcpSentCallback);
    tcp_err(pcb, tcpErrCallback);
    tcp_poll(pcb, tcpPollCallback, 2);
    auto* raw = st.get();
    { std::lock_guard<std::mutex> lk(connMutex_); connections_[pcb] = std::move(st); }
    return raw;
}

void WintunLwipBridge::destroyConnection(TcpConnState* st) {
    if (!st) return;
    std::lock_guard<std::mutex> lk(connMutex_);
    if (st->pcb) { connections_.erase(st->pcb); }
    if (st->socksSocket != INVALID_SOCKET) closeSock(st->socksSocket);
}

// ── SOCKS5 ─────────────────────────────────────────────────────────────

bool WintunLwipBridge::socks5Greet(SOCKET sock) {
    uint8_t greet[] = {0x05, 0x01, 0x00};
    if (!sendExact(sock, greet, 3)) return false;
    uint8_t resp[2];
    if (!recvExact(sock, resp, 2)) return false;
    return resp[0] == 0x05 && resp[1] == 0x00;
}

bool WintunLwipBridge::socks5Connect(SOCKET sock, uint32_t ip, uint16_t port) {
    uint8_t req[10] = {0x05, 0x01, 0x00, 0x01};
    uint32_t nip = htonl(ip);
    memcpy(req+4, &nip, 4);
    uint16_t nport = htons(port);
    memcpy(req+8, &nport, 2);
    if (!sendExact(sock, req, 10)) return false;
    uint8_t resp[10];
    if (!recvExact(sock, resp, 10)) return false;
    return resp[1] == 0x00;
}

void WintunLwipBridge::socksRelayThread(TcpConnState* st) {
    std::array<uint8_t, 16*1024> buf{};
    while (running_.load() && !st->remoteClosed && !st->localClosed) {
        int n = ::recv(st->socksSocket, (char*)buf.data(), (int)buf.size(), 0);
        if (n <= 0) { st->remoteClosed = true; break; }
        {
            std::lock_guard<std::mutex> lk(connMutex_);
            if (st->pendingFromSocksSize + (size_t)n > TcpConnState::kMaxPendingSize) { st->remoteClosed = true; break; }
            st->pendingFromSocks.emplace_back(buf.begin(), buf.begin()+n);
            st->pendingFromSocksSize += n;
        }
    }
    closeSock(st->socksSocket);
    st->socksSocket = INVALID_SOCKET;
}

// ── readThread ─────────────────────────────────────────────────────────

void WintunLwipBridge::readThread() {
    HANDLE ev = WintunGetReadWaitEvent_(session_);
    while (running_.load()) {
        // Flush pending from SOCKS5 to lwIP
        {
            std::lock_guard<std::mutex> lk(connMutex_);
            for (auto& kv : connections_) {
                auto* st = kv.second.get();
                if (!st->pcb || st->localClosed || st->remoteClosed) continue;
                while (!st->pendingFromSocks.empty()) {
                    auto& b = st->pendingFromSocks.front();
                    err_t wr = tcp_write(st->pcb, b.data(), b.size(), TCP_WRITE_FLAG_COPY);
                    if (wr == ERR_OK) { st->pendingFromSocksSize -= b.size(); st->pendingFromSocks.pop_front(); tcp_output(st->pcb); }
                    else break;
                }
            }
        }

        // Read packets from wintun
        while (running_.load()) {
            DWORD sz = 0;
            const uint8_t* pkt = WintunReceivePacket_(session_, &sz);
            if (pkt) { handlePacket(pkt, sz); WintunReleaseReceivePacket_(session_, pkt); }
            else break;
        }

        if (!running_.load()) break;
        WaitForSingleObject(ev, 50);
    }
}

// ── handlePacket ───────────────────────────────────────────────────────

void WintunLwipBridge::handlePacket(const uint8_t* data, size_t len) {
    if (!data || len < sizeof(Ipv4HeaderPacked)) return;
    auto* ip = (const Ipv4HeaderPacked*)data;
    uint8_t ihl = ip->headerLength();
    uint16_t total = ntohs(ip->total_length);
    if (ihl < 20 || total < ihl || total > len) return;

    switch (ip->protocol) {
    case 1:  // ICMP
    case 6:  // TCP
        feedPacketToLwip(data, total);
        break;
    case 17: { // UDP
        if (total < ihl + sizeof(UdpHeaderPacked)) return;
        auto* udp = (const UdpHeaderPacked*)(data + ihl);
        if (ntohs(udp->dst_port) == 53) {
            handleDnsQuery(data, total, ntohl(ip->src_addr), ntohs(udp->src_port), ntohl(ip->dst_addr), ntohs(udp->dst_port));
        }
        break;
    }
    }
}

// ── DNS ────────────────────────────────────────────────────────────────

void WintunLwipBridge::handleDnsQuery(const uint8_t* data, size_t total, uint32_t srcIp, uint16_t srcPort, uint32_t, uint16_t) {
    auto* ip = (const Ipv4HeaderPacked*)data;
    size_t ihl = ip->headerLength();
    const uint8_t* udpStart = data + ihl;
    const uint8_t* dnsStart = udpStart + sizeof(UdpHeaderPacked);
    size_t dnsLen = total - ihl - sizeof(UdpHeaderPacked);
    if (dnsLen < 12) return;
    auto* dh = (DnsHeader*)dnsStart;
    uint16_t dnsId = ntohs(dh->id);
    if (ntohs(dh->qdcount) != 1) return;
    std::string host;
    int nameLen = parseDnsName(dnsStart, dnsLen, 12, host);
    if (nameLen < 0 || host.empty()) return;
    auto ips = resolveHost(host);
    if (ips.empty()) return;
    std::vector<uint8_t> queryData(dnsStart, dnsStart + dnsLen);
    sendDnsReply(srcIp, srcPort, dnsId, queryData, ips[0]);
}

void WintunLwipBridge::sendDnsReply(uint32_t srcIp, uint16_t srcPort, uint16_t dnsId, const std::vector<uint8_t>& query, const std::string& ipStr) {
    size_t ipHdrLen = sizeof(Ipv4HeaderPacked);
    size_t udpHdrLen = sizeof(UdpHeaderPacked);

    // Build DNS reply
    std::vector<uint8_t> dns(512, 0);
    auto* dh = (DnsHeader*)dns.data();
    dh->id = htons(dnsId);
    dh->flags = htons(0x8180); // QR+RD+RA
    dh->qdcount = htons(1);
    dh->ancount = htons(1);
    size_t qpos = 12;
    memcpy(dns.data()+qpos, query.data()+12, query.size()-12);
    size_t qTail = query.size();
    dns[qTail] = 0xC0; dns[qTail+1] = 12;
    size_t apos = qTail + 2;
    // TYPE=A
    dns[apos++] = 0; dns[apos++] = 1;
    // CLASS=IN
    dns[apos++] = 0; dns[apos++] = 1;
    // TTL=60
    dns[apos++] = 0; dns[apos++] = 0; dns[apos++] = 0; dns[apos++] = 60;
    // RDLENGTH=4
    dns[apos++] = 0; dns[apos++] = 4;
    uint32_t addr = inet_addr(ipStr.c_str());
    memcpy(dns.data()+apos, &addr, 4);
    apos += 4;
    size_t dnsSize = apos;

    size_t totalLen = ipHdrLen + udpHdrLen + dnsSize;
    std::vector<uint8_t> pkt(totalLen, 0);
    auto* ipH = (Ipv4HeaderPacked*)pkt.data();
    ipH->version_ihl = 0x45;
    ipH->total_length = htons((uint16_t)totalLen);
    ipH->identification = htons(12345);
    ipH->flags_fragment = htons(0x4000);
    ipH->ttl = 64;
    ipH->protocol = 17;
    ipH->src_addr = htonl(tunIp4_);
    ipH->dst_addr = htonl(srcIp);
    ipH->header_checksum = htons(ipCsum(pkt.data(), ipHdrLen));

    auto* udpH = (UdpHeaderPacked*)(pkt.data() + ipHdrLen);
    udpH->src_port = htons(53);
    udpH->dst_port = htons(srcPort);
    udpH->length = htons((uint16_t)(udpHdrLen + dnsSize));
    udpH->checksum = 0;

    memcpy(pkt.data() + ipHdrLen + udpHdrLen, dns.data(), dnsSize);
    writeTunPacket(pkt.data(), pkt.size());
}

// ── feedPacketToLwip ───────────────────────────────────────────────────

void WintunLwipBridge::feedPacketToLwip(const uint8_t* data, size_t len) {
    if (!lwipNetif_ || !lwipInitialized_) return;
    struct pbuf* p = pbuf_alloc(PBUF_RAW, (uint16_t)len, PBUF_POOL);
    if (!p) return;
    memcpy(p->payload, data, len);
    lwipNetif_->input(p, lwipNetif_);
}

// ── writeTunPacket ────────────────────────────────────────────────────

bool WintunLwipBridge::writeTunPacket(const uint8_t* data, size_t len) {
    if (!session_ || !running_.load() || len == 0) return false;
    std::lock_guard<std::mutex> lk(tunMutex_);
    uint8_t* out = WintunAllocateSendPacket_(session_, (DWORD)len);
    if (!out) return false;
    memcpy(out, data, len);
    WintunSendPacket_(session_, out);
    return true;
}

// ── Helpers ─────────────────────────────────────────────────────────────

uint16_t WintunLwipBridge::ipCsum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i += 2) {
        uint16_t w;
        if (i+1 < len) w = ((uint16_t)data[i] << 8) | data[i+1];
        else w = (uint16_t)data[i] << 8;
        sum += w;
        if (sum > 0xFFFF) sum -= 0xFFFF;
    }
    return (uint16_t)(~sum & 0xFFFF);
}

std::string WintunLwipBridge::ipToStr(uint32_t ip) {
    char buf[INET_ADDRSTRLEN]; uint32_t n = htonl(ip);
    inet_ntop(AF_INET, &n, buf, sizeof(buf));
    return buf;
}
