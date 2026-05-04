#include "ton_transport.hpp"

#include <iostream>

namespace {
class TonTransportAdnl final : public ITonTransport {
public:
    TonTransportAdnl(std::string library_path,
                     std::string private_key,
                     std::string listen_address,
                     uint16_t listen_port);
    ~TonTransportAdnl() override;

    bool start() override;
    void stop() override;
    bool sendToClient(const std::string& client_id, const std::vector<uint8_t>& payload) override;
    std::string localAddress() const override;
    void setOnMessage(
        std::function<void(const std::string& client_id, const std::vector<uint8_t>& payload)> handler) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace

#ifdef __linux__
#include <dlfcn.h>

#include <mutex>

struct TonTransportAdnl::Impl {
    using create_fn = void* (*)(const char*, const char*, uint16_t);
    using start_fn = int (*)(void*);
    using stop_fn = void (*)(void*);
    using destroy_fn = void (*)(void*);
    using local_address_fn = const char* (*)(void*);
    using send_fn = int (*)(void*, const char*, const uint8_t*, size_t);
    using on_message_fn = void (*)(const char*, const uint8_t*, size_t, void*);
    using set_handler_fn = void (*)(void*, on_message_fn, void*);

    std::string library_path;
    std::string private_key;
    std::string listen_address;
    uint16_t listen_port = 0;

    void* so = nullptr;
    void* handle = nullptr;
    create_fn create = nullptr;
    start_fn start = nullptr;
    stop_fn stop = nullptr;
    destroy_fn destroy = nullptr;
    local_address_fn local_address = nullptr;
    send_fn send = nullptr;
    set_handler_fn set_handler = nullptr;

    std::function<void(const std::string&, const std::vector<uint8_t>&)> on_message;
    std::mutex mu;

    static void onMessageBridge(const char* client_id, const uint8_t* data, size_t len, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (!self || !client_id || !data) return;
        std::lock_guard<std::mutex> lock(self->mu);
        if (!self->on_message) return;
        std::vector<uint8_t> payload(data, data + len);
        self->on_message(client_id, payload);
    }

    bool loadSymbols() {
        so = ::dlopen(library_path.c_str(), RTLD_NOW);
        if (!so) return false;
        create = reinterpret_cast<create_fn>(::dlsym(so, "ton_adnl_create"));
        start = reinterpret_cast<start_fn>(::dlsym(so, "ton_adnl_start"));
        stop = reinterpret_cast<stop_fn>(::dlsym(so, "ton_adnl_stop"));
        destroy = reinterpret_cast<destroy_fn>(::dlsym(so, "ton_adnl_destroy"));
        local_address = reinterpret_cast<local_address_fn>(::dlsym(so, "ton_adnl_local_address"));
        send = reinterpret_cast<send_fn>(::dlsym(so, "ton_adnl_send"));
        set_handler = reinterpret_cast<set_handler_fn>(::dlsym(so, "ton_adnl_set_on_message"));
        return create && start && stop && destroy && local_address && send && set_handler;
    }
};

TonTransportAdnl::TonTransportAdnl(std::string library_path,
                                   std::string private_key,
                                   std::string listen_address,
                                   uint16_t listen_port) {
    impl_ = std::make_unique<Impl>();
    impl_->library_path = std::move(library_path);
    impl_->private_key = std::move(private_key);
    impl_->listen_address = std::move(listen_address);
    impl_->listen_port = listen_port;
}

TonTransportAdnl::~TonTransportAdnl() {
    stop();
    if (impl_->so) {
        ::dlclose(impl_->so);
        impl_->so = nullptr;
    }
}

bool TonTransportAdnl::start() {
    if (!impl_->loadSymbols()) {
        std::cerr << "failed to load ADNL backend library or symbols: " << impl_->library_path << std::endl;
        return false;
    }
    impl_->handle = impl_->create(impl_->private_key.c_str(), impl_->listen_address.c_str(), impl_->listen_port);
    if (!impl_->handle) return false;
    impl_->set_handler(impl_->handle, &Impl::onMessageBridge, impl_.get());
    if (impl_->start(impl_->handle) != 0) return false;
    std::cout << "server started" << std::endl;
    std::cout << "ADNL public key/address: " << localAddress() << std::endl;
    std::cout << "egress_adnl_address=" << localAddress() << std::endl;
    return true;
}

void TonTransportAdnl::stop() {
    if (!impl_->handle) return;
    impl_->stop(impl_->handle);
    impl_->destroy(impl_->handle);
    impl_->handle = nullptr;
}

bool TonTransportAdnl::sendToClient(const std::string& client_id, const std::vector<uint8_t>& payload) {
    if (!impl_->handle) return false;
    return impl_->send(impl_->handle, client_id.c_str(), payload.data(), payload.size()) == 0;
}

std::string TonTransportAdnl::localAddress() const {
    if (!impl_->handle || !impl_->local_address) return {};
    const char* addr = impl_->local_address(impl_->handle);
    return addr ? std::string(addr) : std::string();
}

void TonTransportAdnl::setOnMessage(
    std::function<void(const std::string& client_id, const std::vector<uint8_t>& payload)> handler) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->on_message = std::move(handler);
}

#else
struct TonTransportAdnl::Impl {};
TonTransportAdnl::TonTransportAdnl(std::string, std::string, std::string, uint16_t) {
    impl_ = std::make_unique<Impl>();
}
TonTransportAdnl::~TonTransportAdnl() = default;
bool TonTransportAdnl::start() { return false; }
void TonTransportAdnl::stop() {}
bool TonTransportAdnl::sendToClient(const std::string&, const std::vector<uint8_t>&) { return false; }
std::string TonTransportAdnl::localAddress() const { return {}; }
void TonTransportAdnl::setOnMessage(std::function<void(const std::string&, const std::vector<uint8_t>&)>) {}
#endif

std::unique_ptr<ITonTransport> createTonTransport(const std::string& backend,
                                                  const std::string& library_path,
                                                  const std::string& private_key,
                                                  const std::string& listen_address,
                                                  uint16_t listen_port) {
    if (backend == "adnl") {
        auto adnl = std::make_unique<TonTransportAdnl>(library_path, private_key, listen_address, listen_port);
        return adnl;
    }
    std::cout << "unknown transport backend, using stub: " << backend << std::endl;
    return std::make_unique<TonTransportStub>("stub-adnl-address");
}
