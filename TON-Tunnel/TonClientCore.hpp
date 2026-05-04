#pragma once

#include "TonProtocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class ITransportBackend {
public:
    virtual ~ITransportBackend() = default;
    virtual int sendPacket(const std::vector<uint8_t>& packet) = 0;
    virtual bool receivePacket(std::vector<uint8_t>& outPacket, int timeoutMs) = 0;
};

class LoopbackBackend final : public ITransportBackend {
public:
    int sendPacket(const std::vector<uint8_t>& packet) override;
    bool receivePacket(std::vector<uint8_t>& outPacket, int timeoutMs) override;

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> queue_;
};

class TonClientCore {
public:
    struct Counters {
        int64_t openOk{0};
        int64_t openFail{0};
        int64_t openTimeout{0};
        int64_t txBytes{0};
        int64_t rxBytes{0};
    };

    explicit TonClientCore(std::shared_ptr<ITransportBackend> backend);
    ~TonClientCore();

    bool start();
    void stop();

    void registerStream(uint32_t streamId);
    void unregisterStream(uint32_t streamId);

    int openStream(uint32_t streamId, const std::string& host, uint16_t port, const std::string& token);
    int sendStreamData(uint32_t streamId, const std::vector<uint8_t>& payload);
    int closeStream(uint32_t streamId);

    std::optional<InboundFrame> awaitStreamFrame(uint32_t streamId, int timeoutMs);
    Counters snapshotCounters() const;

private:
    void receiverLoop();

    std::shared_ptr<ITransportBackend> backend_;
    std::atomic<bool> running_{false};
    std::thread receiverThread_;

    mutable std::mutex mu_;
    std::unordered_map<uint32_t, std::deque<InboundFrame>> streamQueues_;
    std::unordered_map<uint32_t, int64_t> pendingOpenMs_;
    std::condition_variable cv_;

    std::atomic<int64_t> openOk_{0};
    std::atomic<int64_t> openFail_{0};
    std::atomic<int64_t> openTimeout_{0};
    std::atomic<int64_t> txBytes_{0};
    std::atomic<int64_t> rxBytes_{0};
};

