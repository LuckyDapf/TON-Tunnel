#include <atomic>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "adnl/adnl-address-list.h"
#include "adnl/adnl-network-manager.h"
#include "adnl/adnl.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "keys/keys.hpp"
#include "keyring/keyring.h"
#include "td/actor/actor.h"
#include "td/utils/buffer.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/int_types.h"
#include "td/utils/tl_storers.h"
#include "tl-utils/tl-utils.hpp"

namespace {

// td::IPAddress / ADNL reject UDP port 0; bind an explicit local port (same value as AdnlNetworkManager::create).
constexpr td::uint16 k_local_udp_listen_port = 40000;

static std::string hex_head(const td::Slice &slice, size_t max_bytes = 64) {
  const size_t n = std::min(max_bytes, static_cast<size_t>(slice.size()));
  return td::hex_encode(slice.substr(0, n));
}

constexpr bool kDebugVerbose = false;

static std::string trim_ascii_ws(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  return s.substr(i);
}

static bool is_unusable_advertise_host(const std::string &host) {
  if (host.empty()) {
    return false;
  }
  const auto lower = [](std::string v) {
    for (auto &c : v) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v;
  };
  const auto h = lower(host);
  return h == "0.0.0.0" || h == "127.0.0.1" || h == "localhost" || h == "::1";
}

static bool parse_ipv4_u32(const std::string &ip, uint32_t &out) {
  in_addr addr{};
  if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
    return false;
  }
  out = ntohl(addr.s_addr);
  return true;
}

static bool is_private_ipv4(uint32_t v) {
  const bool is10 = (v & 0xFF000000U) == 0x0A000000U;
  const bool is172 = (v & 0xFFF00000U) == 0xAC100000U;
  const bool is192 = (v & 0xFFFF0000U) == 0xC0A80000U;
  return is10 || is172 || is192;
}

static bool same_subnet24(uint32_t a, uint32_t b) {
  return (a & 0xFFFFFF00U) == (b & 0xFFFFFF00U);
}

using client_callback_fn = void (*)(const uint8_t *, size_t, void *);

class TonAdnlClientRuntime;
void deliver_client_inbound(TonAdnlClientRuntime *owner, td::BufferSlice data);

/**
 * AdnlExtClient (TCP) opens SocketFd to host:port — incompatible with UDP-only egress.
 * Stack mirrors ton-egress-node/src/ton_adnl_backend.cpp: Adnl + AdnlNetworkManager + UDP.
 */
class ClientAdnlInboundCallback final : public ton::adnl::Adnl::Callback {
 public:
  explicit ClientAdnlInboundCallback(TonAdnlClientRuntime *owner) : owner_(owner) {}

  void receive_message(ton::adnl::AdnlNodeIdShort /*src*/, ton::adnl::AdnlNodeIdShort /*dst*/,
                       td::BufferSlice data) override {
    auto s = data.as_slice();
    if (kDebugVerbose) {
      LOG(INFO) << "ClientAdnlInboundCallback::receive_message len=" << s.size() << " hex=" << hex_head(s);
    }
    deliver_client_inbound(owner_, std::move(data));
  }

  void receive_query(ton::adnl::AdnlNodeIdShort /*src*/, ton::adnl::AdnlNodeIdShort /*dst*/, td::BufferSlice /*data*/,
                     td::Promise<td::BufferSlice> promise) override {
    LOG(WARNING) << "ClientAdnlInboundCallback::receive_query unexpected query on message-only client";
    promise.set_error(td::Status::Error("query mode disabled on client; use receive_message path"));
  }

 private:
  TonAdnlClientRuntime *owner_{nullptr};
};

class TonAdnlClientRuntime final {
 public:
  TonAdnlClientRuntime(std::string private_key, std::string egress_adnl_address, std::string local_advertise_host)
      : private_key_(std::move(private_key))
      , egress_adnl_address_(std::move(egress_adnl_address))
      , local_advertise_host_(trim_ascii_ws(std::move(local_advertise_host))) {
  }

  ~TonAdnlClientRuntime() {
    stop();
  }

  bool start() {
    if (running_.exchange(true)) {
      return true;
    }

    // Suppress third_party TON INFO spam (e.g. adnl-peer-table.cpp:59). Keep WARN/ERROR.
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(WARNING));

    scheduler_ = std::make_unique<td::actor::Scheduler>(std::vector<td::actor::Scheduler::NodeInfo>{{1}});
    scheduler_thread_ = std::thread([this] {
      scheduler_->run_in_context([this] { init_adnl_udp(); });
      while (running_.load()) {
        scheduler_->run(0.05);
      }
      scheduler_->run_in_context([this] { shutdown_adnl_udp(); });
    });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    if (scheduler_thread_.joinable()) {
      scheduler_thread_.join();
    }
    scheduler_.reset();
  }

  bool send(const uint8_t *data, size_t len) {
    if (data == nullptr || len == 0 || !ready_.load() || adnl_.empty()) {
      return false;
    }
    if (len >= 9 && data[0] == 2 /* DATA */) {
      const uint32_t payload_len = (static_cast<uint32_t>(data[5]) << 24) |
                                   (static_cast<uint32_t>(data[6]) << 16) |
                                   (static_cast<uint32_t>(data[7]) << 8) |
                                   static_cast<uint32_t>(data[8]);
      if (payload_len == 0) {
        // Drop silently in stability mode (avoid spam).
        return true;
      }
    }
    auto payload = td::BufferSlice(td::Slice(reinterpret_cast<const char *>(data), len));
    scheduler_->run_in_context([this, payload = std::move(payload)]() mutable {
      auto slice = payload.as_slice();
      if (kDebugVerbose) {
        LOG(INFO) << "send_message len=" << slice.size() << " hex=" << hex_head(slice);
      }
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::send_message, local_id_, remote_short_id_, std::move(payload));
    });
    return true;
  }

  void on_inbound_message(td::BufferSlice data) {
    auto s = data.as_slice();
    if (kDebugVerbose) {
      LOG(INFO) << "on_message received len=" << s.size() << " hex=" << hex_head(s);
    }
    emit_inbound(std::move(data));
  }

  void set_callback(client_callback_fn callback, void *user) {
    std::lock_guard<std::mutex> lock(mu_);
    callback_ = callback;
    callback_user_ = user;
  }

  void set_transport_ready(bool value) {
    ready_.store(value);
  }

  void emit_inbound(td::BufferSlice data) {
    client_callback_fn callback = nullptr;
    void *callback_user = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      callback = callback_;
      callback_user = callback_user_;
    }
    if (callback == nullptr || data.empty()) {
      return;
    }
    auto slice = data.as_slice();
    callback(reinterpret_cast<const uint8_t *>(slice.data()), slice.size(), callback_user);
  }

 private:
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
    // Must match privkeys::Ed25519::export_as_slice() / PrivateKey::import() (ton_api::pk_ed25519).
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

  td::Result<std::pair<ton::adnl::AdnlNodeIdFull, std::string>> parse_egress(const std::string &value) {
    auto pos = value.find('@');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= value.size()) {
      return td::Status::Error("Invalid egress_adnl_address format, expected <pubkey_hex>@<host:port>");
    }
    auto pub_hex = td::Slice(value).substr(0, pos);
    auto host = value.substr(pos + 1);

    td::Bits256 key_bits;
    if (key_bits.from_hex(pub_hex) != 256) {
      return td::Status::Error("Invalid ADNL public key hex in egress_adnl_address");
    }
    ton::PublicKey pub{ton::pubkeys::Ed25519(key_bits)};
    return std::make_pair(ton::adnl::AdnlNodeIdFull(pub), std::move(host));
  }

  void init_adnl_udp() {
    auto parsed = parse_egress(egress_adnl_address_);
    if (parsed.is_error()) {
      LOG(ERROR) << parsed.move_as_error();
      ready_.store(false);
      return;
    }
    auto [remote_full_id, remote_host_port] = parsed.move_as_ok();
    remote_short_id_ = remote_full_id.compute_short_id();
    {
      const auto again = remote_full_id.compute_short_id();
      auto pk_slice = remote_full_id.pubkey().export_as_slice();
      LOG(INFO) << "ton_adnl_client: egress remote_short_id=" << remote_short_id_ << " recompute_match=" << (again == remote_short_id_)
                << " pubkey_tl_hex=" << td::hex_encode(pk_slice);
    }

    keyring_ = ton::keyring::Keyring::create("");
    network_manager_ = ton::adnl::AdnlNetworkManager::create(k_local_udp_listen_port);
    adnl_ = ton::adnl::Adnl::create("", keyring_.get());
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_network_manager, network_manager_.get());

    const auto normalized_key = normalize_private_key_for_import(private_key_);
    auto private_key_result = ton::PrivateKey::import(td::Slice(normalized_key));
    if (private_key_result.is_error()) {
      LOG(ERROR) << "ton_adnl_client: failed to import private key: " << private_key_result.move_as_error();
      ready_.store(false);
      return;
    }
    local_private_key_ = private_key_result.move_as_ok();
    local_full_id_ = ton::adnl::AdnlNodeIdFull(local_private_key_.compute_public_key());
    local_id_ = local_full_id_.compute_short_id();

    td::actor::send_closure(
        keyring_, &ton::keyring::Keyring::add_key, local_private_key_, true,
        [](td::Result<td::Unit> result) {
          if (result.is_error()) {
            LOG(ERROR) << "ton_adnl_client: keyring add_key failed: " << result.move_as_error();
          }
        });

    td::IPAddress bind_udp_ip;
    bind_udp_ip.init_ipv4_port("0.0.0.0", k_local_udp_listen_port).ensure();

    td::IPAddress peer_ip;
    auto rs = peer_ip.init_host_port(remote_host_port);
    if (rs.is_error()) {
      LOG(ERROR) << "ton_adnl_client: bad egress host:port " << remote_host_port << ": " << rs;
      ready_.store(false);
      return;
    }

    ton::adnl::AdnlAddressList local_addrs;
    local_addrs.set_version(static_cast<td::uint32>(td::Clocks::system()));
    std::string advertise_mode = "none";
    std::string advertised_address = "<empty>";
    td::IPAddress advertise_udp_ip;
    const std::string trimmed_adv = trim_ascii_ws(local_advertise_host_);
    if (!trimmed_adv.empty()) {
      if (is_unusable_advertise_host(trimmed_adv)) {
        LOG(ERROR) << "ton_adnl_client: invalid local_advertise_host \"" << trimmed_adv << "\"";
        ready_.store(false);
        return;
      }
      auto adv_rs = advertise_udp_ip.init_ipv4_port(trimmed_adv, k_local_udp_listen_port);
      if (adv_rs.is_error()) {
        LOG(ERROR) << "ton_adnl_client: bad local_advertise_host \"" << trimmed_adv << "\": " << adv_rs;
        ready_.store(false);
        return;
      }
      const std::string advertise_ip = advertise_udp_ip.get_ip_str().str();
      const std::string peer_ip_str = peer_ip.get_ip_str().str();
      uint32_t adv_v = 0;
      uint32_t peer_v = 0;
      const bool adv_is_ipv4 = parse_ipv4_u32(advertise_ip, adv_v);
      const bool peer_is_ipv4 = parse_ipv4_u32(peer_ip_str, peer_v);
      const bool adv_is_private = adv_is_ipv4 && is_private_ipv4(adv_v);
      const bool peer_is_private = peer_is_ipv4 && is_private_ipv4(peer_v);
      const bool same_private_subnet = adv_is_private && peer_is_private && same_subnet24(adv_v, peer_v);
      const bool advertises_peer_endpoint = adv_is_ipv4 && peer_is_ipv4 && adv_v == peer_v;
      // Android clients behind NAT must not advertise a guessed public endpoint, because the
      // external UDP source port is typically rewritten and replies get blackholed.
      const bool allow_advertise = same_private_subnet && !advertises_peer_endpoint;
      if (allow_advertise) {
        local_addrs.add_udp_adnl_address(advertise_udp_ip).ensure();
        advertised_address = advertise_ip + ":" + std::to_string(advertise_udp_ip.get_port());
        if (adv_is_private && same_private_subnet) {
          advertise_mode = "LAN";
        } else if (peer_is_ipv4 && advertise_ip == peer_ip_str) {
          advertise_mode = "self";
        } else {
          advertise_mode = "public";
        }
      } else if (!adv_is_private) {
        LOG(WARNING) << "ton_adnl_client: skip public advertise host " << advertise_ip
                     << " because Android external UDP port may differ from local listen port";
      } else if (advertises_peer_endpoint) {
        LOG(WARNING) << "ton_adnl_client: skip advertise host " << advertise_ip
                     << " because it matches egress endpoint host";
      }
    }

    // Outbound UDP requires a non-empty category mask matching add_id(..., cat).
    // AdnlNetworkManager::choose_out_iface uses cat_mask.test(cat); empty mask -> "no out rules".
    constexpr td::uint8 k_local_adnl_category = 0;
    ton::adnl::AdnlCategoryMask out_cat_mask;
    out_cat_mask[k_local_adnl_category] = true;
    td::actor::send_closure(network_manager_, &ton::adnl::AdnlNetworkManager::add_self_addr, bind_udp_ip, out_cat_mask,
                            0);
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, local_full_id_, std::move(local_addrs),
                            k_local_adnl_category);

    ton::adnl::AdnlAddressList peer_addrs;
    peer_addrs.add_udp_adnl_address(peer_ip).ensure();
    peer_addrs.set_version(static_cast<td::uint32>(td::Clocks::system()));
    {
      auto peer_tl = peer_addrs.tl();
      LOG(INFO) << "ton_adnl_client: add_peer remote_full_id + UDP addr_list_tl_hex="
                << td::hex_encode(ton::serialize_tl_object(peer_tl.get(), true));
    }
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_peer, local_id_, remote_full_id, std::move(peer_addrs));

    td::actor::send_closure(adnl_, &ton::adnl::Adnl::subscribe, local_id_, std::string(),
                            std::make_unique<ClientAdnlInboundCallback>(this));

    ready_.store(true);
    LOG(INFO) << "ton_adnl_client: add_peer local=" << local_id_ << " remote_short=" << remote_short_id_
              << " udp=" << peer_ip.get_ip_str() << ":" << peer_ip.get_port();
    LOG(INFO) << "ton_adnl_client: OUT RULES ENABLED for peer";
    LOG(INFO) << "ton_adnl_client: UDP bind " << bind_udp_ip.get_ip_str() << ":" << bind_udp_ip.get_port();
    LOG(INFO) << "ton_adnl_client: UDP bind " << bind_udp_ip.get_ip_str() << ":" << bind_udp_ip.get_port()
              << " advertise_in_addr_list " << advertised_address;
    LOG(INFO) << "ton_adnl_client: advertise mode=" << advertise_mode;
    LOG(INFO) << "ton_adnl_client: UDP ADNL stack ready, peer=" << remote_short_id_;

  }

  void shutdown_adnl_udp() {
    ready_.store(false);
    adnl_ = {};
    network_manager_ = {};
    keyring_ = {};
  }

  std::string private_key_;
  std::string egress_adnl_address_;
  std::string local_advertise_host_;

  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};

  std::mutex mu_;
  client_callback_fn callback_{nullptr};
  void *callback_user_{nullptr};

  std::thread scheduler_thread_;
  std::unique_ptr<td::actor::Scheduler> scheduler_;

  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;

  ton::PrivateKey local_private_key_;
  ton::adnl::AdnlNodeIdFull local_full_id_;
  ton::adnl::AdnlNodeIdShort local_id_;
  ton::adnl::AdnlNodeIdShort remote_short_id_;
};

void deliver_client_inbound(TonAdnlClientRuntime *owner, td::BufferSlice data) {
  if (owner == nullptr) {
    return;
  }
  owner->on_inbound_message(std::move(data));
}

}  // namespace

namespace ton {
td::BufferSlice serialize_tl_object(const ton::lite_api::Object *object, bool boxed) {
  if (object == nullptr) {
    return {};
  }
  td::TlStorerCalcLength calc;
  if (boxed) {
    calc.store_int(object->get_id());
  }
  object->store(calc);

  td::BufferSlice out(calc.get_length());
  td::TlStorerUnsafe storer(out.as_slice().ubegin());
  if (boxed) {
    storer.store_int(object->get_id());
  }
  object->store(storer);
  return out;
}
}  // namespace ton

#if defined(_WIN32)
#define TON_ADNL_CLIENT_API extern "C" __declspec(dllexport)
#else
#define TON_ADNL_CLIENT_API extern "C"
#endif

TON_ADNL_CLIENT_API void *ton_adnl_client_create(const char *private_key, const char *egress_adnl_address,
                                                 const char *local_advertise_ipv4_or_empty) {
  if (!private_key || !egress_adnl_address) return nullptr;
  try {
    const char *adv = local_advertise_ipv4_or_empty != nullptr ? local_advertise_ipv4_or_empty : "";
    return new TonAdnlClientRuntime(private_key, egress_adnl_address, adv);
  } catch (...) {
    return nullptr;
  }
}

TON_ADNL_CLIENT_API int ton_adnl_client_start(void *handle) {
  if (!handle) return -1;
  auto *runtime = static_cast<TonAdnlClientRuntime *>(handle);
  return runtime->start() ? 0 : -2;
}

TON_ADNL_CLIENT_API void ton_adnl_client_stop(void *handle) {
  if (!handle) return;
  static_cast<TonAdnlClientRuntime *>(handle)->stop();
}

TON_ADNL_CLIENT_API void ton_adnl_client_destroy(void *handle) {
  if (!handle) return;
  auto *runtime = static_cast<TonAdnlClientRuntime *>(handle);
  runtime->stop();
  delete runtime;
}

TON_ADNL_CLIENT_API int ton_adnl_client_send(void *handle, const uint8_t *data, size_t len) {
  if (!handle || !data || len == 0) return -1;
  auto *runtime = static_cast<TonAdnlClientRuntime *>(handle);
  return runtime->send(data, len) ? 0 : -2;
}

TON_ADNL_CLIENT_API void ton_adnl_client_set_on_message(void *handle, client_callback_fn callback, void *user_data) {
  if (!handle) return;
  auto *runtime = static_cast<TonAdnlClientRuntime *>(handle);
  runtime->set_callback(callback, user_data);
}

#undef TON_ADNL_CLIENT_API
