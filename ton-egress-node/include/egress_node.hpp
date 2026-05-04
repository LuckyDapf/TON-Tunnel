#pragma once

#include "config.hpp"
#include "ton_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

class EgressNode {
public:
    EgressNode(Config config, std::unique_ptr<ITonTransport> transport);
    ~EgressNode();

    bool start();
    void stop();
    void runUntilInterrupted();

private:
    struct Stream;
    using StreamPtr = std::shared_ptr<Stream>;

    void onAdnlMessage(const std::string& client_id, const std::vector<uint8_t>& payload);
    void handleOpen(const std::string& client_id, uint32_t stream_id, const std::string& host, uint16_t port, const std::string& token);
    void handleData(const std::string& client_id, uint32_t stream_id, const std::vector<uint8_t>& payload);
    void handleClose(const std::string& client_id, uint32_t stream_id);
    /** Removes stream and notifies client unless already absent. Returns true if a stream entry existed. */
    bool closeStream(const std::string& client_id, uint32_t stream_id, const std::string& reason);
    void monitorIdleTimeoutLoop();

    bool isClientAllowed(const std::string& client_id, const std::string& token) const;
    bool isPortAllowed(uint16_t port) const;
    bool isHostBlocked(const std::string& host) const;
    bool isAddressBlocked(const std::string& ip) const;
    std::string keyFor(const std::string& client_id, uint32_t stream_id) const;

private:
    struct Stream {
        std::string client_id;
        uint32_t stream_id = 0;
        int fd = -1;
        std::atomic<bool> closed{false};
        std::atomic<int64_t> last_activity_ms{0};
        std::thread rx_thread;
    };

    Config config_;
    std::unique_ptr<ITonTransport> transport_;
    std::atomic<bool> running_{false};
    std::thread idle_thread_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, StreamPtr> streams_;
    std::unordered_map<std::string, uint32_t> streams_per_client_;
    std::unordered_map<std::string, int64_t> recently_closed_streams_ms_;
};
