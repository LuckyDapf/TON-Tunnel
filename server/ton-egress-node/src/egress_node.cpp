#include "egress_node.hpp"

#include "protocol.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>

namespace {
constexpr bool kDebugVerbose = false;
constexpr int64_t kRecentlyClosedGraceMs = 2000;
constexpr size_t kMaxDataChunkBytes = 1200;
constexpr int kTcpConnectTimeoutMs = 3000;
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void logPayloadHead(const std::vector<uint8_t>& p) {
    std::cout << "ADNL query payload len=" << p.size();
    if (kDebugVerbose) {
        std::cout << " head_hex=";
        const size_t n = std::min<size_t>(p.size(), 32u);
        for (size_t i = 0; i < n; ++i) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned int>(p[i]));
            std::cout << buf;
        }
    }
    if (!p.empty()) {
        std::cout << " first_type=" << static_cast<int>(p[0]);
    }
    std::cout << std::endl;
}

std::string hexHead(const std::vector<uint8_t>& data, size_t max_bytes = 32) {
    const size_t n = std::min(max_bytes, data.size());
    std::string out;
    out.reserve(n * 2 + (data.size() > n ? 1 : 0));
    static const char* kHex = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    if (data.size() > n) out.push_back('…');
    return out;
}

bool tryGetFrameLength(const std::vector<uint8_t>& input, size_t offset, size_t& outLen) {
    if (offset + 5 > input.size()) return false;
    const uint8_t type = input[offset];
    size_t cursor = offset + 5;
    if (type == static_cast<uint8_t>(MessageType::Open)) {
        if (cursor + 2 > input.size()) return false;
        const uint16_t hostLen = static_cast<uint16_t>((input[cursor] << 8) | input[cursor + 1]);
        cursor += 2;
        if (cursor + hostLen + 2 + 2 > input.size()) return false;
        cursor += hostLen + 2;
        const uint16_t tokenLen = static_cast<uint16_t>((input[cursor] << 8) | input[cursor + 1]);
        cursor += 2;
        if (cursor + tokenLen > input.size()) return false;
        outLen = (cursor + tokenLen) - offset;
        return true;
    }
    if (type == static_cast<uint8_t>(MessageType::Data)) {
        if (cursor + 4 > input.size()) return false;
        const uint32_t payloadLen = (static_cast<uint32_t>(input[cursor]) << 24) |
                                    (static_cast<uint32_t>(input[cursor + 1]) << 16) |
                                    (static_cast<uint32_t>(input[cursor + 2]) << 8) |
                                    static_cast<uint32_t>(input[cursor + 3]);
        cursor += 4;
        if (cursor + payloadLen > input.size()) return false;
        outLen = (cursor + payloadLen) - offset;
        return true;
    }
    if (type == static_cast<uint8_t>(MessageType::Close)) {
        outLen = 5;
        return true;
    }
    if (type == static_cast<uint8_t>(MessageType::Error)) {
        if (cursor + 2 > input.size()) return false;
        const uint16_t msgLen = static_cast<uint16_t>((input[cursor] << 8) | input[cursor + 1]);
        cursor += 2;
        if (cursor + msgLen > input.size()) return false;
        outLen = (cursor + msgLen) - offset;
        return true;
    }
    return false;
}

int connectIPv4WithTimeout(const std::string& ip, uint16_t port, int timeout_ms) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        // Connected immediately.
        (void)fcntl(fd, F_SETFL, flags);
        return fd;
    }
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int sel = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
    if (sel <= 0) {
        ::close(fd);
        return -1;
    }

    int so_error = 0;
    socklen_t so_len = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
        ::close(fd);
        return -1;
    }
    (void)fcntl(fd, F_SETFL, flags);
    return fd;
}
} // namespace

namespace {
std::atomic<int64_t> g_open_ok{0};
std::atomic<int64_t> g_open_fail{0};
std::atomic<int64_t> g_tx_bytes{0};
std::atomic<int64_t> g_rx_bytes{0};
std::atomic<int64_t> g_active_streams{0};
std::atomic<int64_t> g_close_remote_closed{0};
std::atomic<int64_t> g_close_idle_timeout{0};
std::atomic<int64_t> g_close_client_close{0};
std::atomic<int64_t> g_close_tcp_write_failed{0};
std::atomic<int64_t> g_close_push_failed{0};
std::atomic<int64_t> g_close_rx_exception{0};
std::atomic<int64_t> g_close_rx_unknown_exception{0};
int64_t g_last_counters_ms = 0;

void maybeLogCounters() {
    const int64_t now = nowMs();
    if (g_last_counters_ms == 0) g_last_counters_ms = now;
    if (now - g_last_counters_ms < 5000) return;
    const auto ok = g_open_ok.exchange(0);
    const auto fail = g_open_fail.exchange(0);
    const auto tx = g_tx_bytes.exchange(0);
    const auto rx = g_rx_bytes.exchange(0);
    const auto active = g_active_streams.load();
    const auto close_remote = g_close_remote_closed.exchange(0);
    const auto close_idle = g_close_idle_timeout.exchange(0);
    const auto close_client = g_close_client_close.exchange(0);
    const auto close_tcp_write = g_close_tcp_write_failed.exchange(0);
    const auto close_push = g_close_push_failed.exchange(0);
    const auto close_rx_ex = g_close_rx_exception.exchange(0);
    const auto close_rx_unknown = g_close_rx_unknown_exception.exchange(0);
    std::cout << "counters open_ok=" << ok << " open_fail=" << fail << " tx_bytes=" << tx << " rx_bytes=" << rx
              << " active_streams=" << active
              << " close_remote=" << close_remote
              << " close_idle=" << close_idle
              << " close_client=" << close_client
              << " close_tcp_write=" << close_tcp_write
              << " close_push=" << close_push
              << " close_rx_ex=" << close_rx_ex
              << " close_rx_unknown=" << close_rx_unknown
              << std::endl;
    g_last_counters_ms = now;
}
} // namespace

EgressNode::EgressNode(Config config, std::unique_ptr<ITonTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {}

EgressNode::~EgressNode() {
    stop();
}

bool EgressNode::start() {
    transport_->setOnMessage([this](const std::string& client, const std::vector<uint8_t>& payload) {
        onAdnlMessage(client, payload);
    });
    if (!transport_->start()) {
        return false;
    }
    running_.store(true);
    idle_thread_ = std::thread([this] { monitorIdleTimeoutLoop(); });
    return true;
}

void EgressNode::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    transport_->stop();
    if (idle_thread_.joinable()) {
        idle_thread_.join();
    }
    // Snapshot streams and release mu_ before join — rx_thread may call closeStream() which locks mu_.
    std::vector<StreamPtr> streams_snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        streams_snapshot.reserve(streams_.size());
        for (auto& it : streams_) {
            streams_snapshot.push_back(it.second);
        }
        streams_.clear();
        streams_per_client_.clear();
    }
    for (auto& stream : streams_snapshot) {
        stream->closed.store(true);
        if (stream->fd >= 0) {
            ::shutdown(stream->fd, SHUT_RDWR);
            ::close(stream->fd);
            stream->fd = -1;
        }
        if (stream->rx_thread.joinable()) {
            stream->rx_thread.join();
        }
    }
}

void EgressNode::runUntilInterrupted() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void EgressNode::onAdnlMessage(const std::string& client_id, const std::vector<uint8_t>& payload) {
    try {
        g_rx_bytes.fetch_add(static_cast<int64_t>(payload.size()));
        if (kDebugVerbose) {
            std::cout << "ADNL query received client=" << client_id << std::endl;
            logPayloadHead(payload);
        }
        size_t offset = 0;
        while (offset < payload.size()) {
            size_t frameLen = 0;
            if (!tryGetFrameLength(payload, offset, frameLen) || frameLen == 0) {
                std::cerr << "ERROR reason=invalid message format at offset=" << offset << std::endl;
                return;
            }
            std::vector<uint8_t> frame(payload.begin() + static_cast<long long>(offset),
                                       payload.begin() + static_cast<long long>(offset + frameLen));
            const auto msg = decodeMessage(frame);
            if (!msg) {
                std::cerr << "ERROR reason=invalid frame decode at offset=" << offset << std::endl;
                return;
            }
            if (msg->type == MessageType::Open && msg->open) {
                if (kDebugVerbose) {
                    std::cout << "parsed command OPEN streamId=" << msg->open->stream_id << " host=" << msg->open->host
                              << " port=" << static_cast<int>(msg->open->port) << std::endl;
                }
                handleOpen(client_id, msg->open->stream_id, msg->open->host, msg->open->port, msg->open->token);
            } else if (msg->type == MessageType::Data && msg->data) {
                if (kDebugVerbose) {
                    std::cout << "parsed command DATA streamId=" << msg->data->stream_id << std::endl;
                }
                handleData(client_id, msg->data->stream_id, msg->data->payload);
            } else if (msg->type == MessageType::Close && msg->close) {
                if (kDebugVerbose) {
                    std::cout << "parsed command CLOSE streamId=" << msg->close->stream_id << std::endl;
                }
                handleClose(client_id, msg->close->stream_id);
            }
            offset += frameLen;
        }
        maybeLogCounters();
    } catch (const std::exception& ex) {
        std::cerr << "onAdnlMessage exception: " << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "onAdnlMessage unknown exception" << std::endl;
    }
}

void EgressNode::handleOpen(
    const std::string& client_id,
    uint32_t stream_id,
    const std::string& host,
    uint16_t port,
    const std::string& token) {
    try {
    if (kDebugVerbose) {
        std::cout << "OPEN streamId=" << stream_id << " " << host << ":" << port << std::endl;
    }
    if (!config_.egress.enabled) {
        transport_->sendToClient(client_id, encodeError({stream_id, "egress disabled"}));
        return;
    }
    if (host.empty() || host.size() > 255) {
        transport_->sendToClient(client_id, encodeError({stream_id, "invalid host"}));
        return;
    }
    if (!isPortAllowed(port)) {
        transport_->sendToClient(client_id, encodeError({stream_id, "port is not allowed"}));
        return;
    }
    if (isHostBlocked(host)) {
        transport_->sendToClient(client_id, encodeError({stream_id, "host is blocked"}));
        return;
    }
    if (!config_.auth_required) {
        if (kDebugVerbose) {
            std::cout << "auth disabled: accepting client client_id=" << client_id << std::endl;
        }
    } else {
        if (!isClientAllowed(client_id, token)) {
            if (!config_.auth_token.empty() && token != config_.auth_token) {
                std::cout << "auth failed: expected token mismatch client_id=" << client_id << std::endl;
            } else if (!config_.allowed_clients.empty()) {
                std::cout << "auth failed: client not in allowed_clients client_id=" << client_id << std::endl;
            } else {
                std::cout << "auth failed: unauthorized client_id=" << client_id << std::endl;
            }
            transport_->sendToClient(client_id, encodeError({stream_id, "unauthorized"}));
            g_open_fail.fetch_add(1);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (streams_per_client_[client_id] >= config_.max_streams) {
            transport_->sendToClient(client_id, encodeError({stream_id, "max streams reached"}));
            return;
        }
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        transport_->sendToClient(client_id, encodeError({stream_id, "DNS resolve failed"}));
        return;
    }
    int fd = -1;
    bool blocked_only = true;
    std::string connected_ip;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        auto* in = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        char ip_buf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &in->sin_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
            continue;
        }
        const std::string ip(ip_buf);
        if (config_.egress.block_private_ips && isAddressBlocked(ip)) {
            continue;
        }
        blocked_only = false;
        fd = connectIPv4WithTimeout(ip, port, kTcpConnectTimeoutMs);
        if (fd >= 0) {
            connected_ip = ip;
            break;
        }
    }
    freeaddrinfo(result);

    if (fd < 0) {
        if (blocked_only) {
            transport_->sendToClient(client_id, encodeError({stream_id, "destination blocked"}));
            return;
        }
        transport_->sendToClient(client_id, encodeError({stream_id, "tcp connect failed"}));
        g_open_fail.fetch_add(1);
        return;
    }
    if (kDebugVerbose) {
        std::cout << "DNS/CONNECT ok " << host << " -> " << connected_ip
                  << " timeout_ms=" << kTcpConnectTimeoutMs << std::endl;
    }
    {
        const int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    if (kDebugVerbose) {
        std::cout << "TCP connected streamId=" << stream_id << std::endl;
    }
    g_open_ok.fetch_add(1);

    auto stream = std::make_shared<Stream>();
    stream->client_id = client_id;
    stream->stream_id = stream_id;
    stream->fd = fd;
    stream->last_activity_ms.store(nowMs());

    {
        std::lock_guard<std::mutex> lock(mu_);
        streams_[keyFor(client_id, stream_id)] = stream;
        streams_per_client_[client_id] += 1;
        g_active_streams.store(static_cast<int64_t>(streams_.size()));
    }
    // OPEN ACK must be immediate and standalone (never batched with DATA).
    const auto open_ack = encodeOpen({stream_id});
    const bool open_ack_ok = transport_->sendToClient(client_id, open_ack);
    g_tx_bytes.fetch_add(static_cast<int64_t>(open_ack.size()));
    if (kDebugVerbose) {
        std::cout << "send OPEN_ACK streamId=" << stream_id
                  << " ok=" << (open_ack_ok ? "true" : "false")
                  << " len=" << open_ack.size();
        if (kDebugVerbose) {
            std::cout << " head_hex=" << hexHead(open_ack);
        }
        std::cout << std::endl;
    }

    stream->rx_thread = std::thread([this, stream] {
        std::vector<uint8_t> buf(16 * 1024);
        while (!stream->closed.load()) {
            try {
                const int readN = static_cast<int>(::recv(stream->fd, buf.data(), buf.size(), 0));
                if (readN <= 0) {
                    closeStream(stream->client_id, stream->stream_id, "remote closed");
                    break;
                }
                stream->last_activity_ms.store(nowMs());
                // Stability mode: push immediately (no batching/timers), keep only 1200B segmentation.
                const size_t total = static_cast<size_t>(readN);
                bool ok = true;
                for (size_t offset = 0; offset < total; offset += kMaxDataChunkBytes) {
                    const size_t len = std::min(kMaxDataChunkBytes, total - offset);
                    std::vector<uint8_t> chunk(buf.begin() + static_cast<long long>(offset),
                                               buf.begin() + static_cast<long long>(offset + len));
                    const auto out = encodeData({stream->stream_id, std::move(chunk)});
                    ok = transport_->sendToClient(stream->client_id, out);
                    g_tx_bytes.fetch_add(static_cast<int64_t>(out.size()));
                    if (!ok) {
                        std::cerr << "DATA push failed streamId=" << stream->stream_id
                                  << " chunk_bytes=" << len << " chunk_index=" << (offset / kMaxDataChunkBytes)
                                  << std::endl;
                        break;
                    }
                }
                if (!ok) {
                    closeStream(stream->client_id, stream->stream_id, "push failed");
                    break;
                }
            } catch (const std::exception& ex) {
                std::cerr << "rx_thread exception streamId=" << stream->stream_id << ": " << ex.what() << std::endl;
                closeStream(stream->client_id, stream->stream_id, "rx exception");
                break;
            } catch (...) {
                std::cerr << "rx_thread unknown exception streamId=" << stream->stream_id << std::endl;
                closeStream(stream->client_id, stream->stream_id, "rx unknown exception");
                break;
            }
        }
    });
    } catch (const std::exception& ex) {
        std::cerr << "handleOpen exception streamId=" << stream_id << ": " << ex.what() << std::endl;
        transport_->sendToClient(client_id, encodeError({stream_id, std::string("internal: ") + ex.what()}));
    } catch (...) {
        std::cerr << "handleOpen unknown exception streamId=" << stream_id << std::endl;
        transport_->sendToClient(client_id, encodeError({stream_id, "internal error"}));
    }
}

void EgressNode::handleData(const std::string& client_id, uint32_t stream_id, const std::vector<uint8_t>& payload) {
    try {
        if (payload.size() > config_.egress.max_payload_bytes) {
            transport_->sendToClient(client_id, encodeError({stream_id, "payload exceeds max_payload_bytes"}));
            return;
        }
        StreamPtr stream;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = streams_.find(keyFor(client_id, stream_id));
            if (it == streams_.end()) {
                const auto now = nowMs();
                auto closed_it = recently_closed_streams_ms_.find(keyFor(client_id, stream_id));
                if (closed_it != recently_closed_streams_ms_.end() && (now - closed_it->second) <= kRecentlyClosedGraceMs) {
                    if (kDebugVerbose) {
                        std::cout << "(debug) late DATA dropped streamId=" << stream_id << std::endl;
                    }
                    return;
                }
                transport_->sendToClient(client_id, encodeError({stream_id, "stream not found"}));
                return;
            }
            stream = it->second;
        }
        if (stream->fd < 0 || stream->closed.load()) {
            transport_->sendToClient(client_id, encodeError({stream_id, "stream closed"}));
            return;
        }
        if (payload.empty()) {
            // Poll path is disabled. Server pushes downstream DATA proactively.
            return;
        }

        const int sent = static_cast<int>(::send(stream->fd, payload.data(), payload.size(), 0));
        if (sent <= 0) {
            closeStream(client_id, stream_id, "tcp write failed");
            return;
        }
        stream->last_activity_ms.store(nowMs());
        if (kDebugVerbose) {
            std::cout << "DATA received streamId=" << stream_id << " size=" << sent << std::endl;
        }

    } catch (const std::exception& ex) {
        std::cerr << "handleData exception streamId=" << stream_id << ": " << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "handleData unknown exception streamId=" << stream_id << std::endl;
    }
}

void EgressNode::handleClose(const std::string& client_id, uint32_t stream_id) {
    try {
        const bool had_stream = closeStream(client_id, stream_id, "client close");
        if (!had_stream) {
            // Client CLOSE for unknown stream: still answer the ADNL query so the client does not hang.
            transport_->sendToClient(client_id, encodeClose({stream_id}));
        }
    } catch (const std::exception& ex) {
        std::cerr << "handleClose exception streamId=" << stream_id << ": " << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "handleClose unknown exception streamId=" << stream_id << std::endl;
    }
}

bool EgressNode::closeStream(const std::string& client_id, uint32_t stream_id, const std::string& reason) {
    try {
        if (reason == "remote closed") g_close_remote_closed.fetch_add(1);
        else if (reason == "idle timeout") g_close_idle_timeout.fetch_add(1);
        else if (reason == "client close") g_close_client_close.fetch_add(1);
        else if (reason == "tcp write failed") g_close_tcp_write_failed.fetch_add(1);
        else if (reason == "push failed") g_close_push_failed.fetch_add(1);
        else if (reason == "rx exception") g_close_rx_exception.fetch_add(1);
        else if (reason == "rx unknown exception") g_close_rx_unknown_exception.fetch_add(1);
        StreamPtr stream;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = streams_.find(keyFor(client_id, stream_id));
            if (it == streams_.end()) {
                recently_closed_streams_ms_[keyFor(client_id, stream_id)] = nowMs();
                return false;
            }
            stream = it->second;
            streams_.erase(it);
            recently_closed_streams_ms_[keyFor(client_id, stream_id)] = nowMs();
            auto countIt = streams_per_client_.find(client_id);
            if (countIt != streams_per_client_.end() && countIt->second > 0) {
                countIt->second -= 1;
            }
        }
        if (!stream->closed.exchange(true)) {
            if (stream->fd >= 0) {
                ::shutdown(stream->fd, SHUT_RDWR);
                ::close(stream->fd);
                stream->fd = -1;
            }
            transport_->sendToClient(client_id, encodeClose({stream_id}));
            if (kDebugVerbose) {
                std::cout << "CLOSE streamId=" << stream_id << " reason=" << reason << std::endl;
            }
        }
        // Must end joinable state before Stream is destroyed: ~std::thread() calls terminate() if joinable().
        if (stream->rx_thread.joinable()) {
            if (stream->rx_thread.get_id() != std::this_thread::get_id()) {
                stream->rx_thread.join();
            } else {
                // Running on rx_thread: cannot join self. Detach so destructor is safe; OS thread exits with this lambda.
                stream->rx_thread.detach();
            }
        }
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "closeStream exception streamId=" << stream_id << ": " << ex.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "closeStream unknown exception streamId=" << stream_id << std::endl;
        return false;
    }
}

void EgressNode::monitorIdleTimeoutLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const int64_t now = nowMs();
        std::vector<std::pair<std::string, uint32_t>> stale;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& [k, stream] : streams_) {
                const int64_t idle = now - stream->last_activity_ms.load();
                if (idle > static_cast<int64_t>(config_.idle_timeout_sec) * 1000) {
                    stale.emplace_back(stream->client_id, stream->stream_id);
                }
            }
            for (auto it = recently_closed_streams_ms_.begin(); it != recently_closed_streams_ms_.end();) {
                if ((now - it->second) > kRecentlyClosedGraceMs) {
                    it = recently_closed_streams_ms_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& [client, streamId] : stale) {
            try {
                closeStream(client, streamId, "idle timeout");
            } catch (const std::exception& ex) {
                std::cerr << "idle closeStream exception: " << ex.what() << std::endl;
            } catch (...) {
                std::cerr << "idle closeStream unknown exception" << std::endl;
            }
        }
    }
}

bool EgressNode::isClientAllowed(const std::string& client_id, const std::string& token) const {
    if (!config_.auth_token.empty() && token != config_.auth_token) {
        return false;
    }
    if (config_.allowed_clients.empty()) {
        return true;
    }
    for (const auto& allowed : config_.allowed_clients) {
        if (allowed == client_id) {
            return true;
        }
    }
    return false;
}

bool EgressNode::isPortAllowed(uint16_t port) const {
    for (const auto allowed : config_.egress.allowed_ports) {
        if (allowed == port) {
            return true;
        }
    }
    return false;
}

bool EgressNode::isHostBlocked(const std::string& host) const {
    std::string lowered;
    lowered.reserve(host.size());
    for (char c : host) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered == "localhost" || lowered == "localhost.localdomain";
}

bool EgressNode::isAddressBlocked(const std::string& ip) const {
    if (ip == "127.0.0.1" || ip == "169.254.169.254") return true;

    in_addr addr{};
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) return true;
    const uint32_t v = ntohl(addr.s_addr);
    const bool is10 = (v & 0xFF000000U) == 0x0A000000U;
    const bool is172 = (v & 0xFFF00000U) == 0xAC100000U;
    const bool is192 = (v & 0xFFFF0000U) == 0xC0A80000U;
    const bool isLoopback = (v & 0xFF000000U) == 0x7F000000U;
    const bool isLinkLocal = (v & 0xFFFF0000U) == 0xA9FE0000U;  // 169.254.0.0/16
    return is10 || is172 || is192 || isLoopback || isLinkLocal;
}

std::string EgressNode::keyFor(const std::string& client_id, uint32_t stream_id) const {
    return client_id + "#" + std::to_string(stream_id);
}
