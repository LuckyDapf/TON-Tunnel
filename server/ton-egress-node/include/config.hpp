#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Config {
    struct Egress {
        bool enabled = true;
        uint32_t max_payload_bytes = 65536;
        std::vector<uint16_t> allowed_ports = {80, 443, 8080, 8443, 853};
        bool block_private_ips = true;
    };

    std::string transport_backend = "adnl";
    std::string adnl_library_path = "./libton_adnl_backend.so";
    std::string adnl_listen_address = "0.0.0.0";
    std::string private_key;
    uint16_t listen_port = 30303;
    uint32_t max_streams = 100;
    uint32_t idle_timeout_sec = 60;
    std::vector<std::string> allowed_clients;
    std::string auth_token;
    // Dev-safe default: auth is optional unless explicitly required.
    bool auth_required = false;
    Egress egress;
};

Config loadConfigOrThrow(const std::string& path);
