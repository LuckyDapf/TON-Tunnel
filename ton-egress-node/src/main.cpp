#include "config.hpp"
#include "egress_node.hpp"
#include "ton_transport.hpp"

#include <atomic>
#include <csignal>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> gStop{false};

void onSignal(int) {
    gStop.store(true);
}
} // namespace

int main(int argc, char** argv) {
    std::string configPath = "../config.json";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        }
    }

    try {
        const Config cfg = loadConfigOrThrow(configPath);
        auto transport = createTonTransport(
            cfg.transport_backend,
            cfg.adnl_library_path,
            cfg.private_key,
            cfg.adnl_listen_address,
            cfg.listen_port);
        EgressNode node(cfg, std::move(transport));
        if (!node.start()) {
            std::cerr << "failed to start egress node" << std::endl;
            return 1;
        }

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        while (!gStop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        node.stop();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << std::endl;
        return 2;
    }
}
