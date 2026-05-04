#include <jni.h>
#include <android/log.h>
#include <tun2socks/tun2socks.h>
#include <vector>
#include <string>

namespace {
constexpr const char* kTag = "tun2socks_bridge";
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_dapf_tongate_data_vpn_Tun2SocksNative_startTun2SocksNative(
        JNIEnv* env,
        jobject /*thiz*/,
        jobjectArray args) {
    if (args == nullptr) {
        return -1;
    }

    const jsize argc = env->GetArrayLength(args);
    if (argc <= 0) {
        return -2;
    }

    std::vector<std::string> storage;
    storage.reserve(static_cast<size_t>(argc));

    std::vector<char*> argv;
    argv.reserve(static_cast<size_t>(argc));

    for (jsize i = 0; i < argc; ++i) {
        auto jArg = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        if (jArg == nullptr) {
            return -3;
        }
        const char* utf = env->GetStringUTFChars(jArg, nullptr);
        if (utf == nullptr) {
            env->DeleteLocalRef(jArg);
            return -4;
        }
        storage.emplace_back(utf);
        env->ReleaseStringUTFChars(jArg, utf);
        env->DeleteLocalRef(jArg);
    }

    for (auto& item : storage) {
        argv.push_back(item.data());
    }

    __android_log_print(ANDROID_LOG_INFO, kTag, "tun2socks_start argc=%d", static_cast<int>(argc));
    return tun2socks_start(static_cast<int>(argc), argv.data());
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_dapf_tongate_data_vpn_Tun2SocksNative_stopTun2SocksNative(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "tun2socks_terminate");
    tun2socks_terminate();
}
