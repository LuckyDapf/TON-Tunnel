#include "config.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace {
std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open config: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string extractString(const std::string& src, const std::string& key, const std::string& fallback = "") {
    const std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (std::regex_search(src, m, re) && m.size() > 1) {
        return m[1].str();
    }
    return fallback;
}

uint32_t extractUint(const std::string& src, const std::string& key, uint32_t fallback) {
    const std::regex re("\"" + key + "\"\\s*:\\s*([0-9]+)");
    std::smatch m;
    if (std::regex_search(src, m, re) && m.size() > 1) {
        return static_cast<uint32_t>(std::stoul(m[1].str()));
    }
    return fallback;
}

bool extractBool(const std::string& src, const std::string& key, bool fallback) {
    const std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (std::regex_search(src, m, re) && m.size() > 1) {
        return m[1].str() == "true";
    }
    return fallback;
}

std::vector<std::string> extractStringArray(const std::string& src, const std::string& key) {
    const std::regex listRe("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch listMatch;
    std::vector<std::string> out;
    if (!std::regex_search(src, listMatch, listRe) || listMatch.size() < 2) {
        return out;
    }

    const std::string body = listMatch[1].str();
    const std::regex itemRe("\"([^\"]*)\"");
    auto begin = std::sregex_iterator(body.begin(), body.end(), itemRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        out.push_back((*it)[1].str());
    }
    return out;
}

std::vector<uint16_t> extractUint16Array(const std::string& src, const std::string& key) {
    const std::regex listRe("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch listMatch;
    std::vector<uint16_t> out;
    if (!std::regex_search(src, listMatch, listRe) || listMatch.size() < 2) {
        return out;
    }
    const std::string body = listMatch[1].str();
    const std::regex itemRe("([0-9]+)");
    auto begin = std::sregex_iterator(body.begin(), body.end(), itemRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const auto value = static_cast<uint32_t>(std::stoul((*it)[1].str()));
        if (value <= 65535) {
            out.push_back(static_cast<uint16_t>(value));
        }
    }
    return out;
}
} // namespace

Config loadConfigOrThrow(const std::string& path) {
    const std::string raw = readAll(path);
    Config cfg;
    cfg.transport_backend = extractString(raw, "transport_backend", cfg.transport_backend);
    cfg.adnl_library_path = extractString(raw, "adnl_library_path", cfg.adnl_library_path);
    cfg.adnl_library_path = extractString(raw, "adnl_backend_library", cfg.adnl_library_path);
    cfg.adnl_listen_address = extractString(raw, "adnl_listen_address", cfg.adnl_listen_address);
    cfg.private_key = extractString(raw, "private_key");
    cfg.listen_port = static_cast<uint16_t>(extractUint(raw, "listen_port", cfg.listen_port));
    cfg.max_streams = extractUint(raw, "max_streams", cfg.max_streams);
    cfg.idle_timeout_sec = extractUint(raw, "idle_timeout_sec", cfg.idle_timeout_sec);
    cfg.allowed_clients = extractStringArray(raw, "allowed_clients");
    cfg.auth_token = extractString(raw, "auth_token", "");
    cfg.auth_required = extractBool(raw, "auth_required", cfg.auth_required);

    cfg.egress.enabled = extractBool(raw, "enabled", cfg.egress.enabled);
    cfg.egress.max_payload_bytes = extractUint(raw, "max_payload_bytes", cfg.egress.max_payload_bytes);
    cfg.egress.block_private_ips = extractBool(raw, "block_private_ips", cfg.egress.block_private_ips);
    const auto ports = extractUint16Array(raw, "allowed_ports");
    if (!ports.empty()) {
        cfg.egress.allowed_ports = ports;
    }
    return cfg;
}
