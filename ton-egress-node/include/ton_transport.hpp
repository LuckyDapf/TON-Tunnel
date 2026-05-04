#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ITonTransport {
public:
    virtual ~ITonTransport() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool sendToClient(const std::string& client_id, const std::vector<uint8_t>& payload) = 0;
    virtual std::string localAddress() const = 0;
    virtual void setOnMessage(
        std::function<void(const std::string& client_id, const std::vector<uint8_t>& payload)> handler) = 0;
};

class TonTransportStub final : public ITonTransport {
public:
    explicit TonTransportStub(std::string local_address);
    bool start() override;
    void stop() override;
    bool sendToClient(const std::string& client_id, const std::vector<uint8_t>& payload) override;
    std::string localAddress() const override;
    void setOnMessage(
        std::function<void(const std::string& client_id, const std::vector<uint8_t>& payload)> handler) override;

private:
    std::string local_address_;
    std::function<void(const std::string&, const std::vector<uint8_t>&)> on_message_;
};

std::unique_ptr<ITonTransport> createTonTransport(const std::string& backend,
                                                  const std::string& library_path,
                                                  const std::string& private_key,
                                                  const std::string& listen_address,
                                                  uint16_t listen_port);
