#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace {
constexpr const char* kTag = "TonTransportJNI";

using adnl_on_message_fn = void (*)(const uint8_t*, size_t, void*);

extern "C" {
void* ton_adnl_client_create(const char* private_key, const char* egress_adnl_address,
                             const char* local_advertise_ipv4_or_empty);
int ton_adnl_client_start(void* handle);
void ton_adnl_client_stop(void* handle);
void ton_adnl_client_destroy(void* handle);
int ton_adnl_client_send(void* handle, const uint8_t* data, size_t len);
void ton_adnl_client_set_on_message(void* handle, adnl_on_message_fn callback, void* user_data);
}

struct AdnlContext {
    void* client_handle{nullptr};
};

std::atomic<bool> gNativeReady{false};
std::atomic<uint64_t> gJniCalls{0};
std::atomic<uint64_t> gJniBytes{0};
std::atomic<uint64_t> gDroppedEmptyFrames{0};
std::atomic<int64_t> gMetricsWindowStartMs{
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count()};
constexpr bool kMetricsEnabled = false;
constexpr int64_t kMetricsWindowMs = 5000;
std::mutex gStateMutex;
std::string gLastError;
std::mutex gQueueMutex;
std::queue<std::vector<uint8_t>> gInboundQueue;
AdnlContext* gContext = nullptr;
std::string gPendingAddress;
std::string gPendingPrivateKey;
std::string gPendingToken;
std::string gPendingLocalAdvertiseHost;

void setLastError(const std::string& message) {
    std::lock_guard<std::mutex> lock(gStateMutex);
    gLastError = message;
}

std::string getLastError() {
    std::lock_guard<std::mutex> lock(gStateMutex);
    return gLastError;
}

void onAdnlMessage(const uint8_t* data, size_t len, void* /*user*/) {
    if (data == nullptr || len == 0) return;
    __android_log_print(ANDROID_LOG_INFO, kTag, "onAdnlMessage: len=%zu", len);
    std::vector<uint8_t> payload(data, data + len);
    std::lock_guard<std::mutex> lock(gQueueMutex);
    gInboundQueue.push(std::move(payload));
    __android_log_print(ANDROID_LOG_INFO, kTag, "onAdnlMessage: queued packet, queue_size=%zu", gInboundQueue.size());
}

void maybeLogMetrics() {
    if (!kMetricsEnabled) return;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const int64_t started = gMetricsWindowStartMs.load();
    const int64_t elapsedMs = nowMs - started;
    if (elapsedMs < kMetricsWindowMs) return;
    const uint64_t calls = gJniCalls.exchange(0);
    const uint64_t bytes = gJniBytes.exchange(0);
    const uint64_t dropped = gDroppedEmptyFrames.exchange(0);
    const double seconds = static_cast<double>(elapsedMs > 0 ? elapsedMs : 1) / 1000.0;
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "metrics jni_calls_per_sec=%.2f tx_bytes_per_sec=%.2f dropped_empty_frames=%llu",
        static_cast<double>(calls) / seconds, static_cast<double>(bytes) / seconds,
        static_cast<unsigned long long>(dropped));
    gMetricsWindowStartMs.store(nowMs);
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_dapf_tongate_data_native_NativeTonTransport_initAdnlContext(
        JNIEnv* env,
        jobject /*thiz*/,
        jstring config,
        jstring keystoreDir) {
    (void)config;
    (void)keystoreDir;
    auto* context = new AdnlContext;
    if (gContext != nullptr) {
        if (gContext->client_handle) {
            ton_adnl_client_destroy(gContext->client_handle);
        }
        delete gContext;
        gContext = nullptr;
    }
    if (gPendingAddress.empty() || gPendingPrivateKey.empty()) {
        setLastError("ADNL peer config is incomplete. Call configureAdnlPeer before initialize.");
        delete context;
        gNativeReady.store(false);
        return 0;
    }
    context->client_handle =
        ton_adnl_client_create(gPendingPrivateKey.c_str(), gPendingAddress.c_str(), gPendingLocalAdvertiseHost.c_str());
    if (!context->client_handle) {
        setLastError("ton_adnl_client_create returned null");
        delete context;
        gNativeReady.store(false);
        return 0;
    }
    ton_adnl_client_set_on_message(context->client_handle, onAdnlMessage, nullptr);
    if (ton_adnl_client_start(context->client_handle) != 0) {
        setLastError("ton_adnl_client_start returned non-zero");
        ton_adnl_client_destroy(context->client_handle);
        delete context;
        gNativeReady.store(false);
        return 0;
    }

    gContext = context;
    __android_log_print(ANDROID_LOG_INFO, kTag, "initAdnlContext: ADNL context=%p", context);
    setLastError("");
    gNativeReady.store(true);
    return reinterpret_cast<jlong>(context);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_dapf_tongate_data_native_NativeTonTransport_configureAdnlPeer(
        JNIEnv* env,
        jobject /*thiz*/,
        jstring egressAdnlAddress,
        jstring clientPrivateKey,
        jstring authToken,
        jstring localUdpAdvertiseHost) {
    const char* addressChars = env->GetStringUTFChars(egressAdnlAddress, nullptr);
    const char* keyChars = env->GetStringUTFChars(clientPrivateKey, nullptr);
    const char* tokenChars = env->GetStringUTFChars(authToken, nullptr);
    const char* advertiseChars =
        localUdpAdvertiseHost != nullptr ? env->GetStringUTFChars(localUdpAdvertiseHost, nullptr) : nullptr;

    gPendingAddress = addressChars != nullptr ? addressChars : "";
    gPendingPrivateKey = keyChars != nullptr ? keyChars : "";
    gPendingToken = tokenChars != nullptr ? tokenChars : "";
    gPendingLocalAdvertiseHost = advertiseChars != nullptr ? advertiseChars : "";

    if (addressChars != nullptr) env->ReleaseStringUTFChars(egressAdnlAddress, addressChars);
    if (keyChars != nullptr) env->ReleaseStringUTFChars(clientPrivateKey, keyChars);
    if (tokenChars != nullptr) env->ReleaseStringUTFChars(authToken, tokenChars);
    if (localUdpAdvertiseHost != nullptr && advertiseChars != nullptr) {
        env->ReleaseStringUTFChars(localUdpAdvertiseHost, advertiseChars);
    }

    __android_log_print(
            ANDROID_LOG_INFO,
            kTag,
            "ADNL peer configured: address=%s keySize=%zu tokenSize=%zu advertiseHost=%s",
            gPendingAddress.c_str(),
            gPendingPrivateKey.size(),
            gPendingToken.size(),
            gPendingLocalAdvertiseHost.c_str()
    );
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_dapf_tongate_data_native_NativeTonTransport_sendPacket(
        JNIEnv* env,
        jobject /*thiz*/,
        jbyteArray packet) {
    if (packet == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "sendPacket: null packet");
        return -1;
    }

    const jsize packetLength = env->GetArrayLength(packet);
    if (packetLength <= 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "sendPacket: empty packet");
        gDroppedEmptyFrames.fetch_add(1);
        maybeLogMetrics();
        return -2;
    }

    jboolean isCopy = JNI_FALSE;
    jbyte* bytes = env->GetByteArrayElements(packet, &isCopy);
    if (bytes == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "sendPacket: failed to map byte array");
        return -3;
    }

    if (!gNativeReady.load() || gContext == nullptr) {
        env->ReleaseByteArrayElements(packet, bytes, JNI_ABORT);
        setLastError("sendPacket: native context is not initialized.");
        return -4;
    }
    const bool sent = ton_adnl_client_send(gContext->client_handle, reinterpret_cast<const uint8_t*>(bytes),
                                           static_cast<size_t>(packetLength)) == 0;
    gJniCalls.fetch_add(1);
    gJniBytes.fetch_add(static_cast<uint64_t>(packetLength));
    maybeLogMetrics();

    __android_log_print(ANDROID_LOG_DEBUG, kTag, "sendPacket: len=%d, isCopy=%d", packetLength, isCopy);
    env->ReleaseByteArrayElements(packet, bytes, JNI_ABORT);
    return sent ? 0 : -5;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_dapf_tongate_data_native_NativeTonTransport_sendPacketDirect(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject packetBuffer,
        jint packetLength) {
    if (packetBuffer == nullptr || packetLength <= 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "sendPacketDirect: invalid input");
        if (packetLength <= 0) {
            gDroppedEmptyFrames.fetch_add(1);
            maybeLogMetrics();
        }
        return -1;
    }

    auto* data = static_cast<uint8_t*>(env->GetDirectBufferAddress(packetBuffer));
    const jlong capacity = env->GetDirectBufferCapacity(packetBuffer);
    if (data == nullptr || capacity < packetLength) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "sendPacketDirect: not a direct buffer or invalid capacity");
        return -2;
    }

    if (!gNativeReady.load() || gContext == nullptr) {
        setLastError("sendPacketDirect: native context is not initialized.");
        return -3;
    }
    const bool sent = ton_adnl_client_send(gContext->client_handle, data, static_cast<size_t>(packetLength)) == 0;
    gJniCalls.fetch_add(1);
    gJniBytes.fetch_add(static_cast<uint64_t>(packetLength));
    maybeLogMetrics();

    __android_log_print(ANDROID_LOG_DEBUG, kTag, "sendPacketDirect: len=%d", packetLength);
    return sent ? 0 : -4;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_example_dapf_tongate_data_native_NativeTonTransport_receivePacket(
        JNIEnv* env,
        jobject /*thiz*/) {
    if (!gNativeReady.load()) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "receivePacket: native context is not initialized");
        return nullptr;
    }

    if (gContext == nullptr || gContext->client_handle == nullptr) {
        setLastError("receivePacket: ADNL context is empty.");
        return nullptr;
    }
    std::vector<uint8_t> nextPacket;
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        if (gInboundQueue.empty()) {
            return nullptr;
        }
        nextPacket = std::move(gInboundQueue.front());
        gInboundQueue.pop();
        __android_log_print(ANDROID_LOG_INFO, kTag, "receivePacket: dequeued len=%zu remaining=%zu", nextPacket.size(),
                            gInboundQueue.size());
    }
    if (nextPacket.empty()) {
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(nextPacket.size()));
    if (result == nullptr) {
        setLastError("Failed to allocate jbyteArray in receivePacket.");
        return nullptr;
    }
    env->SetByteArrayRegion(
            result,
            0,
            static_cast<jsize>(nextPacket.size()),
            reinterpret_cast<const jbyte*>(nextPacket.data())
    );
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_dapf_tongate_data_native_NativeTonTransport_getLastNativeError(
        JNIEnv* env,
        jobject /*thiz*/) {
    const std::string err = getLastError();
    return env->NewStringUTF(err.c_str());
}

