#include "TonClientCore.hpp"

#include <chrono>

namespace {
// Match Android transport behavior: allow enough time for egress connect/auth.
constexpr int kOpenTimeoutMs = 20000;
constexpr int kOpenCloseGraceMs = 250;
constexpr int kDataSegmentSize = 1200;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

int LoopbackBackend::sendPacket(const std::vector<uint8_t>& packet) {
    const auto decoded = decodeInboundFrames(packet);
    if (decoded.empty()) return -1;

    std::vector<uint8_t> response;
    const auto& f = decoded.front();
    if (f.type == FrameType::Open) {
        response = encodeOpenAckFrame(f.streamId);
    } else if (f.type == FrameType::Data) {
        response = encodeDataFrame(f.streamId, f.payload);
    } else if (f.type == FrameType::Close) {
        response = encodeCloseFrame(f.streamId);
    } else if (f.type == FrameType::Error) {
        response = encodeErrorFrame(f.streamId, f.errorMessage.empty() ? "loopback" : f.errorMessage);
    } else {
        return -2;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(std::move(response));
    }
    cv_.notify_one();
    return 0;
}

bool LoopbackBackend::receivePacket(std::vector<uint8_t>& outPacket, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !queue_.empty(); })) {
        return false;
    }
    outPacket = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

TonClientCore::TonClientCore(std::shared_ptr<ITransportBackend> backend)
    : backend_(std::move(backend)) {
}

TonClientCore::~TonClientCore() {
    stop();
}

bool TonClientCore::start() {
    if (!backend_) return false;
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return true;
    receiverThread_ = std::thread([this] { receiverLoop(); });
    return true;
}

void TonClientCore::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    cv_.notify_all();
    if (receiverThread_.joinable()) receiverThread_.join();
}

void TonClientCore::registerStream(uint32_t streamId) {
    std::lock_guard<std::mutex> lock(mu_);
    streamQueues_.try_emplace(streamId);
}

void TonClientCore::unregisterStream(uint32_t streamId) {
    std::lock_guard<std::mutex> lock(mu_);
    streamQueues_.erase(streamId);
    pendingOpenMs_.erase(streamId);
}

int TonClientCore::openStream(uint32_t streamId, const std::string& host, uint16_t port, const std::string& token) {
    try {
        registerStream(streamId);
        auto openFrame = encodeOpenFrame(streamId, host, port, token);
        {
            std::lock_guard<std::mutex> lock(mu_);
            pendingOpenMs_[streamId] = nowMs();
        }
        txBytes_.fetch_add(static_cast<int64_t>(openFrame.size()));
        if (backend_->sendPacket(openFrame) != 0) {
            openFail_.fetch_add(1);
            return -1;
        }

        const int64_t deadline = nowMs() + kOpenTimeoutMs;
        std::optional<int64_t> closeSeenAt;
        while (nowMs() < deadline) {
            int waitMs = static_cast<int>(deadline - nowMs());
            if (closeSeenAt.has_value()) {
                const int64_t remainingClose = kOpenCloseGraceMs - (nowMs() - *closeSeenAt);
                if (remainingClose <= 0) {
                    openFail_.fetch_add(1);
                    return -4;
                }
                waitMs = static_cast<int>(std::min<int64_t>(waitMs, remainingClose));
            }
            auto f = awaitStreamFrame(streamId, waitMs);
            if (!f.has_value()) continue;
            if (f->type == FrameType::Open) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    pendingOpenMs_.erase(streamId);
                }
                openOk_.fetch_add(1);
                return 0;
            }
            if (f->type == FrameType::Error) {
                openFail_.fetch_add(1);
                return -3;
            }
            if (f->type == FrameType::Close) {
                if (!closeSeenAt.has_value()) closeSeenAt = nowMs();
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            pendingOpenMs_.erase(streamId);
        }
        openTimeout_.fetch_add(1);
        return -2;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            pendingOpenMs_.erase(streamId);
        }
        openFail_.fetch_add(1);
        return -99;
    }
}

int TonClientCore::sendStreamData(uint32_t streamId, const std::vector<uint8_t>& payload) {
    if (payload.empty()) return 0;
    size_t offset = 0;
    while (offset < payload.size()) {
        const size_t len = std::min(static_cast<size_t>(kDataSegmentSize), payload.size() - offset);
        std::vector<uint8_t> chunk(payload.begin() + static_cast<long long>(offset), payload.begin() + static_cast<long long>(offset + len));
        auto dataFrame = encodeDataFrame(streamId, chunk);
        txBytes_.fetch_add(static_cast<int64_t>(dataFrame.size()));
        if (backend_->sendPacket(dataFrame) != 0) return -1;
        offset += len;
    }
    return 0;
}

int TonClientCore::closeStream(uint32_t streamId) {
    auto frame = encodeCloseFrame(streamId);
    txBytes_.fetch_add(static_cast<int64_t>(frame.size()));
    {
        std::lock_guard<std::mutex> lock(mu_);
        pendingOpenMs_.erase(streamId);
    }
    return backend_->sendPacket(frame);
}

std::optional<InboundFrame> TonClientCore::awaitStreamFrame(uint32_t streamId, int timeoutMs) {
    try {
        std::unique_lock<std::mutex> lock(mu_);
        auto hasFrame = [this, streamId] {
            auto it = streamQueues_.find(streamId);
            return it != streamQueues_.end() && !it->second.empty();
            };
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), hasFrame)) {
            return std::nullopt;
        }
    auto& q = streamQueues_[streamId];
        if (q.empty()) return std::nullopt;
        InboundFrame f = std::move(q.front());
        q.pop_front();
        return f;
    } catch (...) {
        return std::nullopt;
    }
}

TonClientCore::Counters TonClientCore::snapshotCounters() const {
    return Counters{
        openOk_.load(),
        openFail_.load(),
        openTimeout_.load(),
        txBytes_.load(),
        rxBytes_.load()
    };
}

void TonClientCore::receiverLoop() {
    while (running_.load()) {
        try {
            std::vector<uint8_t> packet;
            if (!backend_->receivePacket(packet, 50)) {
                continue;
            }
            rxBytes_.fetch_add(static_cast<int64_t>(packet.size()));
            auto frames = decodeInboundFrames(packet);
            if (frames.empty()) {
                continue;
            }
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& frame : frames) {
                auto it = streamQueues_.find(frame.streamId);
                if (it == streamQueues_.end()) {
                    if (frame.type == FrameType::Open || frame.type == FrameType::Close || frame.type == FrameType::Error) {
                        it = streamQueues_.try_emplace(frame.streamId).first;
                    } else {
                        continue;
                    }
                }
                it->second.push_back(std::move(frame));
            }
            cv_.notify_all();
        } catch (...) {
            // Keep receiver thread alive even on malformed packet or transient backend exception.
            continue;
        }
    }
}

