#include "LocalSocks5Server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace {
constexpr SOCKET kInvalidSocket = INVALID_SOCKET;
std::atomic<bool> g_tonPathLogged{false};

bool recvExact(SOCKET s, uint8_t* dst, int len) {
    int got = 0;
    while (got < len) {
        const int n = recv(s, reinterpret_cast<char*>(dst + got), len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

void gracefulCloseClient(SOCKET client) {
    if (client == kInvalidSocket) return;
    shutdown(client, SD_SEND);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    closesocket(client);
}

SOCKET connectRemoteTcp(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) {
        return kInvalidSocket;
    }
    SOCKET remote = kInvalidSocket;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket) continue;
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            remote = s;
            break;
        }
        closesocket(s);
    }
    freeaddrinfo(result);
    return remote;
}
} // namespace

LocalSocks5Server::LocalSocks5Server(
    std::shared_ptr<TonClientCore> core,
    std::string authToken,
    std::function<void(const std::string&)> logger
) : core_(std::move(core)), authToken_(std::move(authToken)), logger_(std::move(logger)) {}

LocalSocks5Server::~LocalSocks5Server() {
    stop();
}

bool LocalSocks5Server::start(uint16_t listenPort, std::string& error) {
    if (running_.exchange(true)) return true;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        running_.store(false);
        error = "Socks5: WSAStartup failed";
        return false;
    }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == kInvalidSocket) {
        running_.store(false);
        error = "Socks5: socket() failed";
        WSACleanup();
        return false;
    }

    u_long one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(listenPort);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(ls);
        running_.store(false);
        error = "Socks5: bind() failed on 127.0.0.1:" + std::to_string(listenPort);
        WSACleanup();
        return false;
    }
    if (listen(ls, SOMAXCONN) != 0) {
        closesocket(ls);
        running_.store(false);
        error = "Socks5: listen() failed";
        WSACleanup();
        return false;
    }

    listenSocketRaw_ = static_cast<uintptr_t>(ls);
    listenPort_ = listenPort;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    if (logger_) logger_("SOCKS5 listening on 127.0.0.1:" + std::to_string(listenPort_));
    return true;
}

void LocalSocks5Server::stop() {
    if (!running_.exchange(false)) return;

    SOCKET ls = static_cast<SOCKET>(listenSocketRaw_);
    if (ls != kInvalidSocket) {
        closesocket(ls);
        listenSocketRaw_ = static_cast<uintptr_t>(-1);
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lk(workersMutex_);
        workers.swap(workerThreads_);
    }
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
    WSACleanup();
}

void LocalSocks5Server::acceptLoop() {
    SOCKET ls = static_cast<SOCKET>(listenSocketRaw_);
    while (running_.load()) {
        SOCKET client = accept(ls, nullptr, nullptr);
        if (client == kInvalidSocket) {
            if (!running_.load()) break;
            continue;
        }
        if (logger_) logger_("SOCKS5 accepted client");
        try {
            std::thread worker([this, clientRaw = static_cast<uintptr_t>(client)] {
                if (logger_) logger_("SOCKS5 thread started");
                try {
                    handleClient(clientRaw);
                } catch (const std::exception& ex) {
                    if (logger_) logger_(std::string("SOCKS5 thread exc: ") + ex.what());
                } catch (...) {
                    if (logger_) logger_("SOCKS5 thread exc: unknown");
                }
                if (logger_) logger_("SOCKS5 thread ended");
            });
            std::lock_guard<std::mutex> lk(workersMutex_);
            workerThreads_.push_back(std::move(worker));
        } catch (const std::exception& ex) {
            if (logger_) logger_(std::string("SOCKS5 thread create failed: ") + ex.what());
        } catch (...) {
            if (logger_) logger_("SOCKS5 thread create failed: unknown");
        }
    }
}

bool LocalSocks5Server::sendAll(uintptr_t clientSocketRaw, const uint8_t* data, size_t size) {
    SOCKET s = static_cast<SOCKET>(clientSocketRaw);
    size_t sent = 0;
    while (sent < size) {
        const int n = send(s, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void LocalSocks5Server::handleClient(uintptr_t clientSocketRaw) {
    SOCKET client = static_cast<SOCKET>(clientSocketRaw);
    auto closeNow = [&]() {
        gracefulCloseClient(client);
    };

    // Set 5-second timeout to avoid hanging forever
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    auto bridgeViaBackend = [&](const std::string& targetHost, uint16_t targetPort) {
        if (logger_) logger_("SOCKS CONNECT " + targetHost + ":" + std::to_string(targetPort));

        if (!core_) {
            // Direct TCP fallback (no ADNL)
            SOCKET remote = connectRemoteTcp(targetHost, targetPort);
            if (remote == kInvalidSocket) return false;

            std::atomic<bool> alive{true};
            std::thread uplink([&] {
                std::array<uint8_t, 16 * 1024> buf{};
                while (alive.load() && running_.load()) {
                    const int n = recv(client, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
                    if (n <= 0) break;
                    int sent = 0;
                    while (sent < n) {
                        const int m = send(remote, reinterpret_cast<const char*>(buf.data() + sent), n - sent, 0);
                        if (m <= 0) { alive.store(false); return; }
                        sent += m;
                    }
                }
                alive.store(false);
            });

            std::array<uint8_t, 16 * 1024> downBuf{};
            while (alive.load() && running_.load()) {
                const int n = recv(remote, reinterpret_cast<char*>(downBuf.data()), static_cast<int>(downBuf.size()), 0);
                if (n <= 0) break;
                if (!sendAll(clientSocketRaw, downBuf.data(), static_cast<size_t>(n))) break;
            }
            alive.store(false);
            shutdown(remote, SD_BOTH);
            closesocket(remote);
            closeNow();
            if (uplink.joinable()) uplink.join();
            return true;
        }

        const uint32_t streamId = nextStreamId_.fetch_add(1);
        if (logger_) logger_("OPEN begin stream=" + std::to_string(streamId));
        const int openCode = core_->openStream(streamId, targetHost, targetPort, authToken_);
        if (openCode != 0) {
            if (logger_) logger_("OPEN FAILED code=" + std::to_string(openCode) + " target=" + targetHost + ":" + std::to_string(targetPort));
            core_->unregisterStream(streamId);
            return false;
        }
        if (logger_) logger_("OPEN ok stream=" + std::to_string(streamId));
        if (logger_ && !g_tonPathLogged.exchange(true)) {
            logger_("TON path active");
        }

        std::atomic<bool> alive{true};
        bool remoteEnded = false;
        std::thread uplink([&] {
            std::array<uint8_t, 16 * 1024> buf{};
            while (alive.load() && running_.load()) {
                const int n = recv(client, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
                if (n <= 0) break;
                std::vector<uint8_t> payload(buf.begin(), buf.begin() + n);
                if (core_->sendStreamData(streamId, payload) != 0) break;
            }
            alive.store(false);
        });

        while (alive.load() && running_.load()) {
            auto frame = core_->awaitStreamFrame(streamId, 100);
            if (!frame.has_value()) continue;
            if (frame->type == FrameType::Data) {
                if (!frame->payload.empty()) {
                    if (!sendAll(clientSocketRaw, frame->payload.data(), frame->payload.size())) break;
                }
            } else if (frame->type == FrameType::Close || frame->type == FrameType::Error) {
                remoteEnded = true;
                break;
            }
        }

        alive.store(false);
        closeNow();
        if (uplink.joinable()) uplink.join();
        if (!remoteEnded) (void)core_->closeStream(streamId);
        core_->unregisterStream(streamId);
        return true;
    };

    // SOCKS version/method preface
    uint8_t greetingHead[2];
    if (!recvExact(client, greetingHead, 2)) {
        if (logger_) logger_("SOCKS5 recv greeting failed/timeout");
        closeNow();
        return;
    }
    if (greetingHead[0] == 0x04) {
        // SOCKS4 / SOCKS4a request:
        // VN(0x04), CD(0x01), DSTPORT(2), DSTIP(4), USERID(0), [DOMAIN(0) for 4a]
        const uint8_t cmd = greetingHead[1];
        if (cmd != 0x01) {
            const uint8_t fail[8] = {0x00, 0x5B, 0, 0, 0, 0, 0, 0};
            (void)sendAll(clientSocketRaw, fail, sizeof(fail));
            closeNow();
            return;
        }
        uint8_t portBytes[2]{};
        uint8_t ipBytes[4]{};
        if (!recvExact(client, portBytes, 2) || !recvExact(client, ipBytes, 4)) {
            closeNow();
            return;
        }
        // Skip USERID (zero-terminated string)
        for (int i = 0; i < 512; i++) {
            uint8_t ch = 0;
            if (!recvExact(client, &ch, 1)) { closeNow(); return; }
            if (ch == 0) break;
        }

        std::string targetHost;
        const bool is4a = (ipBytes[0] == 0 && ipBytes[1] == 0 && ipBytes[2] == 0 && ipBytes[3] != 0);
        if (is4a) {
            std::vector<uint8_t> domain;
            for (int i = 0; i < 512; i++) {
                uint8_t ch = 0;
                if (!recvExact(client, &ch, 1)) { closeNow(); return; }
                if (ch == 0) break;
                domain.push_back(ch);
            }
            targetHost.assign(domain.begin(), domain.end());
            if (targetHost.empty()) {
                const uint8_t fail[8] = {0x00, 0x5B, 0, 0, 0, 0, 0, 0};
                (void)sendAll(clientSocketRaw, fail, sizeof(fail));
                closeNow();
                return;
            }
        } else {
            char buf[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, ipBytes, buf, static_cast<socklen_t>(sizeof(buf)));
            targetHost = buf;
        }

        const uint16_t targetPort = static_cast<uint16_t>((portBytes[0] << 8) | portBytes[1]);
        if (logger_) logger_("SOCKS4 CONNECT " + targetHost + ":" + std::to_string(targetPort));
        const uint8_t okReply4[8] = {0x00, 0x5A, 0, 0, 0, 0, 0, 0};
        if (!sendAll(clientSocketRaw, okReply4, sizeof(okReply4))) {
            closeNow();
            return;
        }
        (void)bridgeViaBackend(targetHost, targetPort);
        return;
    }
    if (greetingHead[0] != 0x05) {
        if (logger_) logger_("SOCKS5 bad version " + std::to_string(greetingHead[0]));
        closeNow();
        return;
    }
    std::vector<uint8_t> methods(greetingHead[1]);
    if (!methods.empty() && !recvExact(client, methods.data(), static_cast<int>(methods.size()))) {
        closeNow();
        return;
    }
    const uint8_t methodReply[2] = {0x05, 0x00};
    if (!sendAll(clientSocketRaw, methodReply, sizeof(methodReply))) {
        closeNow();
        return;
    }

    // SOCKS5 request
    uint8_t reqHead[4];
    if (!recvExact(client, reqHead, 4)) {
        if (logger_) logger_("SOCKS5 recv request failed");
        closeNow();
        return;
    }
    if (reqHead[0] != 0x05 || reqHead[1] != 0x01) {
        closeNow();
        return;
    }

    std::string targetHost;
    if (reqHead[3] == 0x01) { // IPv4
        std::array<uint8_t, 4> ip{};
        if (!recvExact(client, ip.data(), 4)) { closeNow(); return; }
        char buf[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, ip.data(), buf, static_cast<socklen_t>(sizeof(buf)));
        targetHost = buf;
    } else if (reqHead[3] == 0x03) { // Domain
        uint8_t len = 0;
        if (!recvExact(client, &len, 1)) { closeNow(); return; }
        std::vector<uint8_t> name(len);
        if (len > 0 && !recvExact(client, name.data(), len)) { closeNow(); return; }
        targetHost.assign(name.begin(), name.end());
    } else if (reqHead[3] == 0x04) { // IPv6
        std::array<uint8_t, 16> ip6{};
        if (!recvExact(client, ip6.data(), static_cast<int>(ip6.size()))) { closeNow(); return; }
        char buf[INET6_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET6, ip6.data(), buf, static_cast<socklen_t>(sizeof(buf))) == nullptr) { closeNow(); return; }
        targetHost = buf;
    } else {
        closeNow();
        return;
    }

    uint8_t portBytes[2];
    if (!recvExact(client, portBytes, 2)) { closeNow(); return; }
    const uint16_t targetPort = static_cast<uint16_t>((portBytes[0] << 8) | portBytes[1]);
    const uint8_t okReply[10] = {0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    if (!sendAll(clientSocketRaw, okReply, sizeof(okReply))) {
        closeNow();
        return;
    }
    (void)bridgeViaBackend(targetHost, targetPort);
}
