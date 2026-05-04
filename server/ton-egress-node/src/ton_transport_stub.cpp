#include "ton_transport.hpp"

#include <iostream>

TonTransportStub::TonTransportStub(std::string local_address) : local_address_(std::move(local_address)) {}

bool TonTransportStub::start() {
    std::cout << "server started" << std::endl;
    std::cout << "ADNL public key/address: " << local_address_ << std::endl;
    std::cout << "WARNING: TonTransportStub is active. Replace with real ADNL transport." << std::endl;
    return true;
}

void TonTransportStub::stop() {
    std::cout << "transport stopped" << std::endl;
}

bool TonTransportStub::sendToClient(const std::string& client_id, const std::vector<uint8_t>& payload) {
    std::cout << "bytes out client=" << client_id << " size=" << payload.size() << std::endl;
    return true;
}

std::string TonTransportStub::localAddress() const {
    return local_address_;
}

void TonTransportStub::setOnMessage(
    std::function<void(const std::string&, const std::vector<uint8_t>&)> handler) {
    on_message_ = std::move(handler);
}
