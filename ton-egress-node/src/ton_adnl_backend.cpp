#include "ton_transport.hpp"

#if defined(__linux__)

#include "adnl/adnl-address-list.h"
#include "adnl/adnl.h"
#include "adnl/adnl-network-manager.h"
#include "auto/tl/ton_api.h"
#include "keyring/keyring.h"
#include "keys/keys.hpp"
#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/IPAddress.h"

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

enum : uint8_t {
  kFrameOpen = 1,
  kFrameData = 2,
  kFrameClose = 3,
  kFrameError = 4,
};

constexpr bool kDebugVerbose = false;

std::atomic<int64_t> g_tx_bytes{0};
std::atomic<int64_t> g_rx_bytes{0};
std::atomic<int64_t> g_open{0};
std::atomic<int64_t> g_close{0};
std::atomic<int64_t> g_error{0};
std::atomic<int64_t> g_data_frames{0};
int64_t g_last_counters_ms = 0;

void maybe_log_counters() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  if (g_last_counters_ms == 0) g_last_counters_ms = now;
  if (now - g_last_counters_ms < 5000) return;
  // Keep counters visible even when global verbosity suppresses INFO logs.
  std::cout << "counters tx_bytes=" << g_tx_bytes.exchange(0) << " rx_bytes=" << g_rx_bytes.exchange(0)
            << " open=" << g_open.exchange(0) << " close=" << g_close.exchange(0) << " error=" << g_error.exchange(0)
            << " data_frames=" << g_data_frames.exchange(0) << std::endl;
  g_last_counters_ms = now;
}

class AdnlBackendRuntime;

class AdnlInboundCallback final : public ton::adnl::Adnl::Callback {
 public:
  explicit AdnlInboundCallback(AdnlBackendRuntime *owner) : owner_(owner) {
  }
  void receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void receive_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override;

 private:
  AdnlBackendRuntime *owner_;
};

class AdnlBackendRuntime final : public ITonTransport {
 public:
  AdnlBackendRuntime(std::string private_key, std::string listen_address, uint16_t listen_port)
      : private_key_(std::move(private_key)), listen_address_(std::move(listen_address)), listen_port_(listen_port) {}

  bool start() override {
    if (running_.exchange(true)) {
      return true;
    }

    scheduler_ = std::make_unique<td::actor::Scheduler>(std::vector<td::actor::Scheduler::NodeInfo>{{1}});
    scheduler_thread_ = std::thread([this] {
      scheduler_->run_in_context([this] { init_adnl_runtime(); });
      while (running_.load()) {
        scheduler_->run(0.05);
      }
      scheduler_->run_in_context([this] { shutdown_adnl_runtime(); });
    });
    return true;
  }

  void stop() override {
    if (!running_.exchange(false)) return;
    if (scheduler_thread_.joinable()) scheduler_thread_.join();
    scheduler_.reset();
  }

  bool sendToClient(const std::string& client_id, const std::vector<uint8_t>& payload) override {
    if (!running_.load() || payload.empty() || adnl_.empty()) {
      return false;
    }
    const auto stream_key = extract_stream_key(client_id, payload);
    bool answered_pending_query = false;
    if (!stream_key.empty()) {
      std::lock_guard<std::mutex> lock(reply_mu_);
      auto pending_it = pending_query_promises_.find(stream_key);
      if (pending_it != pending_query_promises_.end()) {
        pending_it->second.set_value(td::BufferSlice(td::Slice(reinterpret_cast<const char *>(payload.data()), payload.size())));
        pending_query_promises_.erase(pending_it);
        answered_pending_query = true;
      }
    }
    const uint8_t frame_type = payload.empty() ? 0 : payload[0];
    const uint32_t stream_id = payload.size() >= 5 ? read_u32_be(payload, 1) : 0;
    ton::adnl::AdnlNodeIdShort dst;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = known_clients_.find(client_id);
      if (it == known_clients_.end()) {
        LOG(WARNING) << "send_message drop unknown client id";
        return false;
      }
      dst = it->second;
    }
    auto msg = td::BufferSlice(td::Slice(reinterpret_cast<const char *>(payload.data()), payload.size()));
    const size_t payload_len = payload.size() >= 9 ? payload.size() - 9 : 0;
    scheduler_->run_in_context([this, dst, msg = std::move(msg), frame_type, stream_id, payload_len, answered_pending_query]() mutable {
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::send_message, local_id_, dst, std::move(msg));
      if (kDebugVerbose) {
        if (frame_type == kFrameData) {
          LOG(INFO) << "send_message DATA streamId=" << stream_id << " payload_len=" << payload_len
                    << " also_answered_query=" << (answered_pending_query ? "true" : "false");
        } else if (frame_type == kFrameOpen) {
          LOG(INFO) << "send_message OPEN streamId=" << stream_id
                    << " also_answered_query=" << (answered_pending_query ? "true" : "false");
        } else if (frame_type == kFrameClose) {
          LOG(INFO) << "send_message CLOSE streamId=" << stream_id
                    << " also_answered_query=" << (answered_pending_query ? "true" : "false");
        } else if (frame_type == kFrameError) {
          LOG(WARNING) << "send_message ERROR streamId=" << stream_id
                       << " also_answered_query=" << (answered_pending_query ? "true" : "false");
        }
      }
      maybe_log_counters();
    });
    g_tx_bytes.fetch_add(static_cast<int64_t>(payload.size()));
    if (frame_type == kFrameOpen) g_open.fetch_add(1);
    if (frame_type == kFrameClose) g_close.fetch_add(1);
    if (frame_type == kFrameError) g_error.fetch_add(1);
    if (frame_type == kFrameData) g_data_frames.fetch_add(1);
    return true;
  }

  std::string localAddress() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return local_address_public_;
  }

  void setOnMessage(std::function<void(const std::string&, const std::vector<uint8_t>&)> handler) override {
    std::lock_guard<std::mutex> lock(mu_);
    on_message_ = std::move(handler);
  }

  void on_adnl_message(ton::adnl::AdnlNodeIdShort src, td::BufferSlice data) {
    const std::string id_serialized = src.serialize();
    auto slice = data.as_slice();
    g_rx_bytes.fetch_add(static_cast<int64_t>(slice.size()));
    std::vector<uint8_t> payload(reinterpret_cast<const uint8_t *>(slice.data()),
                                 reinterpret_cast<const uint8_t *>(slice.data()) + slice.size());

    std::function<void(const std::string&, const std::vector<uint8_t>&)> handler;
    {
      std::lock_guard<std::mutex> lock(mu_);
      known_clients_[id_serialized] = src;
      handler = on_message_;
    }
    if (handler) {
      handler(id_serialized, payload);
    }
    maybe_log_counters();
  }

  void on_adnl_query(ton::adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
    const std::string client_id = src.serialize();
    auto query_bytes = data.as_slice();
    std::vector<uint8_t> payload(reinterpret_cast<const uint8_t *>(query_bytes.data()),
                                 reinterpret_cast<const uint8_t *>(query_bytes.data()) + query_bytes.size());
    const auto stream_key = extract_stream_key(client_id, payload);
    if (stream_key.empty()) {
      promise.set_error(td::Status::Error("invalid query frame"));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      known_clients_[client_id] = src;
    }
    {
      std::lock_guard<std::mutex> lock(reply_mu_);
      auto it = pending_query_promises_.find(stream_key);
      if (it != pending_query_promises_.end()) {
        it->second.set_error(td::Status::Error("query superseded by new request"));
        pending_query_promises_.erase(it);
      }
      pending_query_promises_.emplace(stream_key, std::move(promise));
    }
    std::function<void(const std::string&, const std::vector<uint8_t>&)> handler;
    {
      std::lock_guard<std::mutex> lock(mu_);
      handler = on_message_;
    }
    if (handler) {
      handler(client_id, payload);
    } else {
      std::lock_guard<std::mutex> lock(reply_mu_);
      auto it = pending_query_promises_.find(stream_key);
      if (it != pending_query_promises_.end()) {
        it->second.set_error(td::Status::Error("egress handler is not set"));
        pending_query_promises_.erase(it);
      }
    }
  }

 private:
  static uint32_t read_u32_be(const std::vector<uint8_t> &payload, size_t offset) {
    return (static_cast<uint32_t>(payload[offset]) << 24) |
           (static_cast<uint32_t>(payload[offset + 1]) << 16) |
           (static_cast<uint32_t>(payload[offset + 2]) << 8) |
           static_cast<uint32_t>(payload[offset + 3]);
  }

  static void write_u32_be(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
  }

  static std::vector<uint8_t> make_empty_data_ack(uint32_t stream_id) {
    std::vector<uint8_t> out;
    out.reserve(1 + 4 + 4);
    out.push_back(kFrameData);
    write_u32_be(out, stream_id);
    write_u32_be(out, 0);
    return out;
  }

  static std::string extract_stream_key(const std::string &client_id, const std::vector<uint8_t> &payload) {
    if (payload.size() < 5) {
      return {};
    }
    const uint32_t stream_id = read_u32_be(payload, 1);
    return client_id + "#" + std::to_string(stream_id);
  }

  static std::vector<uint8_t> maybe_decode_hex(const std::string &value) {
    std::vector<uint8_t> out;
    if ((value.size() % 2) != 0) {
      return out;
    }
    out.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
      auto hex_to_int = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
      };
      const int hi = hex_to_int(value[i]);
      const int lo = hex_to_int(value[i + 1]);
      if (hi < 0 || lo < 0) {
        out.clear();
        return out;
      }
      out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
  }

  static std::string to_tl_pk_ed25519(const std::vector<uint8_t> &raw32) {
    // Must match PrivateKey::import() / ton_api::pk_ed25519 TL id (little-endian int32).
    std::string out;
    out.reserve(4 + raw32.size());
    const td::int32 id = ton::ton_api::pk_ed25519::ID;
    out.append(reinterpret_cast<const char *>(&id), sizeof(id));
    out.append(reinterpret_cast<const char *>(raw32.data()), raw32.size());
    return out;
  }

  static std::string normalize_private_key_for_import(const std::string &value) {
    const auto raw = maybe_decode_hex(value);
    if (raw.empty()) {
      return value;
    }
    if (raw.size() == 36) {
      return std::string(reinterpret_cast<const char *>(raw.data()), raw.size());
    }
    if (raw.size() == 32) {
      return to_tl_pk_ed25519(raw);
    }
    return std::string(reinterpret_cast<const char *>(raw.data()), raw.size());
  }

  static std::string public_key_hex_from_full_id(const ton::adnl::AdnlNodeIdFull &full_id) {
    auto key_bin = full_id.pubkey().export_as_slice();
    auto key_slice = key_bin.as_slice();
    if (key_slice.size() >= 36) {
      key_slice.remove_prefix(4);
    }
    return td::hex_encode(key_slice);
  }

  void init_adnl_runtime() {
    // Suppress third_party TON INFO spam (e.g. adnl-peer-table.cpp:59). Keep WARN/ERROR.
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(WARNING));

    keyring_ = ton::keyring::Keyring::create("");
    network_manager_ = ton::adnl::AdnlNetworkManager::create(listen_port_);
    adnl_ = ton::adnl::Adnl::create("", keyring_.get());
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_network_manager, network_manager_.get());

    const auto normalized_key = normalize_private_key_for_import(private_key_);
    auto private_key_result = ton::PrivateKey::import(td::Slice(normalized_key));
    if (private_key_result.is_error()) {
      LOG(ERROR) << "ton_adnl_backend: failed to import private key: " << private_key_result.move_as_error();
      return;
    }
    local_private_key_ = private_key_result.move_as_ok();
    local_full_id_ = ton::adnl::AdnlNodeIdFull(local_private_key_.compute_public_key());
    local_id_ = local_full_id_.compute_short_id();

    td::actor::send_closure(
        keyring_, &ton::keyring::Keyring::add_key, local_private_key_, true,
        [](td::Result<td::Unit> result) {
          if (result.is_error()) {
            LOG(ERROR) << "ton_adnl_backend: keyring add_key failed: " << result.move_as_error();
          }
        });

    ton::adnl::AdnlAddressList addr_list;
    td::IPAddress addr;
    auto status = addr.init_host_port(listen_address_, listen_port_);
    if (status.is_error()) {
      addr.init_ipv4_port("0.0.0.0", listen_port_).ensure();
    }
    addr_list.add_udp_adnl_address(addr).ensure();
    // Inbound UDP is dropped in AdnlPeerTableImpl::receive_packet if !cat_mask.test(local_id_cat).
    // Empty AdnlCategoryMask() never matches category 0 → packets seen in tcpdump never reach ADNL.
    constexpr td::uint8 k_local_adnl_category = 0;
    ton::adnl::AdnlCategoryMask udp_cat_mask;
    udp_cat_mask[k_local_adnl_category] = true;
    td::actor::send_closure(network_manager_, &ton::adnl::AdnlNetworkManager::add_self_addr, addr, udp_cat_mask, 0);
    addr_list.set_version(static_cast<td::uint32>(td::Clocks::system()));
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, local_full_id_, std::move(addr_list), k_local_adnl_category);
    // Empty prefix matches every query name (see AdnlLocalId::deliver_query); includes "ton_gate.frame".
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::subscribe, local_id_, std::string(),
                            std::make_unique<AdnlInboundCallback>(this));

    {
      std::lock_guard<std::mutex> lock(mu_);
      local_address_public_ =
          public_key_hex_from_full_id(local_full_id_) + "@" + listen_address_ + ":" + std::to_string(listen_port_);
    }
    LOG(INFO) << "ton_adnl_backend: UDP iface category bit " << static_cast<int>(k_local_adnl_category)
              << " set; local_short_id=" << local_id_ << " subscribe_prefix=<empty>(all names e.g. ton_gate.frame) "
              << "listen=" << addr.get_ip_str() << ":" << addr.get_port();
  }

  void shutdown_adnl_runtime() {
    adnl_ = {};
    network_manager_ = {};
    keyring_ = {};
  }

  std::string private_key_;
  std::string listen_address_;
  uint16_t listen_port_{0};

  std::atomic<bool> running_{false};
  std::thread scheduler_thread_;
  std::unique_ptr<td::actor::Scheduler> scheduler_;

  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  ton::PrivateKey local_private_key_;
  ton::adnl::AdnlNodeIdFull local_full_id_;
  ton::adnl::AdnlNodeIdShort local_id_;

  mutable std::mutex mu_;
  std::string local_address_public_;
  std::function<void(const std::string&, const std::vector<uint8_t>&)> on_message_;
  std::unordered_map<std::string, ton::adnl::AdnlNodeIdShort> known_clients_;

  std::mutex reply_mu_;
  std::unordered_map<std::string, td::Promise<td::BufferSlice>> pending_query_promises_;
};

void AdnlInboundCallback::receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort, td::BufferSlice data) {
  if (owner_ != nullptr) {
    owner_->on_adnl_message(src, std::move(data));
  }
}

void AdnlInboundCallback::receive_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort,
                                        td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  if (owner_ != nullptr) {
    owner_->on_adnl_query(src, std::move(data), std::move(promise));
  } else {
    promise.set_error(td::Status::Error("backend runtime not available"));
  }
}

} // namespace

extern "C" {

void* ton_adnl_create(const char* private_key, const char* listen_address, uint16_t listen_port) {
  if (!private_key || !listen_address) return nullptr;
  try {
    return new AdnlBackendRuntime(private_key, listen_address, listen_port);
  } catch (...) {
    return nullptr;
  }
}

int ton_adnl_start(void* handle) {
  if (!handle) return -1;
  auto* runtime = static_cast<AdnlBackendRuntime*>(handle);
  return runtime->start() ? 0 : -2;
}

void ton_adnl_stop(void* handle) {
  if (!handle) return;
  static_cast<AdnlBackendRuntime*>(handle)->stop();
}

void ton_adnl_destroy(void* handle) {
  if (!handle) return;
  auto* runtime = static_cast<AdnlBackendRuntime*>(handle);
  runtime->stop();
  delete runtime;
}

const char* ton_adnl_local_address(void* handle) {
  if (!handle) return nullptr;
  static std::string buffer;
  buffer = static_cast<AdnlBackendRuntime*>(handle)->localAddress();
  return buffer.c_str();
}

int ton_adnl_send(void* handle, const char* client_id, const uint8_t* data, size_t len) {
  if (!handle || !client_id || !data) return -1;
  std::vector<uint8_t> payload(data, data + len);
  return static_cast<AdnlBackendRuntime*>(handle)->sendToClient(client_id, payload) ? 0 : -2;
}

using ton_adnl_cb = void (*)(const char*, const uint8_t*, size_t, void*);

void ton_adnl_set_on_message(void* handle, ton_adnl_cb cb, void* user_data) {
  if (!handle) return;
  auto* runtime = static_cast<AdnlBackendRuntime*>(handle);
  runtime->setOnMessage([cb, user_data](const std::string& client, const std::vector<uint8_t>& payload) {
    if (cb) cb(client.c_str(), payload.data(), payload.size(), user_data);
  });
}

} // extern "C"

#endif // __linux__
