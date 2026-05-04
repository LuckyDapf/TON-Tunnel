#pragma once

#include "TonClientCore.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LocalSocks5Server {
public:
    explicit LocalSocks5Server(
        std::shared_ptr<TonClientCore> core,
        std::string authToken,
        std::function<void(const std::string&)> logger
    );
    ~LocalSocks5Server();

    bool start(uint16_t listenPort, std::string& error);
    void stop();

private:
    void acceptLoop();
    void handleClient(uintptr_t clientSocketRaw);
    bool sendAll(uintptr_t clientSocketRaw, const uint8_t* data, size_t size);

    std::shared_ptr<TonClientCore> core_;
    std::string authToken_;
    std::function<void(const std::string&)> logger_;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::mutex workersMutex_;
    std::vector<std::thread> workerThreads_;
    uintptr_t listenSocketRaw_{static_cast<uintptr_t>(-1)};
    uint16_t listenPort_{0};
    std::atomic<uint32_t> nextStreamId_{1000};
};

