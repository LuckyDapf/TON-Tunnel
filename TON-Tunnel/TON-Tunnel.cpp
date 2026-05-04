// TON-Tunnel.cpp : minimal Android-like START/STOP UI.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <wininet.h>
#ifndef INTERNET_OPTION_PROXY_SETTINGS_CHANGED
#define INTERNET_OPTION_PROXY_SETTINGS_CHANGED 95
#endif

#include "LocalSocks5Server.hpp"
#include "TonClientCore.hpp"
#include "TonProtocol.hpp"
#include "WintunLwipBridge.hpp"

// Direct ADNL client backend API (linked from ton_adnl_client_backend DLL)
extern "C" {
    void* ton_adnl_client_create(const char* private_key, const char* egress_adnl_address,
                                  const char* local_advertise_ipv4_or_empty);
    int ton_adnl_client_start(void* handle);
    void ton_adnl_client_stop(void* handle);
    void ton_adnl_client_destroy(void* handle);
    int ton_adnl_client_send(void* handle, const uint8_t* data, size_t len);
    void ton_adnl_client_set_on_message(void* handle,
        void (*callback)(const uint8_t*, size_t, void*), void* user_data);
}

#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "Psapi.lib")

namespace {

constexpr int kButtonIdStart = 1001;
constexpr int kButtonIdStop = 1002;
constexpr int kLabelIdStatus = 1003;
constexpr int kEditIdLog = 1004;
constexpr UINT kMsgAppendLog = WM_APP + 1;
constexpr UINT kMsgAutoStart = WM_APP + 2;
constexpr int kWindowWidth = 900;
constexpr int kWindowHeight = 560;
constexpr int kButtonWidth = 220;
constexpr int kButtonHeight = 56;

HWND g_startButton = nullptr;
HWND g_stopButton = nullptr;
HWND g_statusLabel = nullptr;
HWND g_logBox = nullptr;

// Direct ADNL backend (linked from ton_adnl_client_backend.dll)
void* g_adnlHandle = nullptr;
std::mutex g_adnlMutex;
std::deque<std::vector<uint8_t>> g_adnlInboundQueue;
std::condition_variable g_adnlCv;
std::atomic<bool> g_adnlStarted{false};

std::shared_ptr<TonClientCore> g_core;
std::unique_ptr<LocalSocks5Server> g_socks5;
std::unique_ptr<WintunLwipBridge> g_bridge;
std::wstring g_logFilePath;
std::wstring g_crashDumpPath;
std::wstring g_proxyStatePath;

struct ProxyState {
    bool captured{false};
    DWORD accessType{INTERNET_OPEN_TYPE_PRECONFIG};
    std::wstring proxy;
    std::wstring bypass;
};
ProxyState g_prevProxy;
void restoreSystemProxy();

struct RuntimeConfig {
    std::string egress;
    std::string clientKey;
    std::string advertiseHost;
    std::string authToken;
    uint16_t socksPort{1080};
};

std::string extractJsonStringField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return "";
    const size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) return "";
    const size_t quoteStart = json.find('"', colonPos + 1);
    if (quoteStart == std::string::npos) return "";
    size_t quoteEnd = quoteStart + 1;
    while (quoteEnd < json.size()) {
        if (json[quoteEnd] == '"' && json[quoteEnd - 1] != '\\') break;
        ++quoteEnd;
    }
    if (quoteEnd >= json.size()) return "";
    return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

std::wstring getExeDir() {
    std::wstring buf(MAX_PATH, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return L".";
    buf.resize(n);
    const size_t slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return buf.substr(0, slash);
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::string narrowUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string readTextFileUtf8(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void appendFileLogLine(const std::wstring& line) {
    if (g_logFilePath.empty()) return;
    std::wofstream out(g_logFilePath, std::ios::app);
    if (!out) return;
    out << line << L"\n";
}

std::wstring widenUtf8(const char* s) {
    if (!s) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

std::wstring formatHex(uint64_t v) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << std::uppercase << v;
    return ss.str();
}

void logExceptionStack(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ContextRecord) return;

    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const std::wstring exeDir = getExeDir();
    const std::string searchPath(exeDir.begin(), exeDir.end());
    if (!SymInitialize(proc, searchPath.c_str(), TRUE)) {
        appendFileLogLine(L"SymInitialize failed gle=" + std::to_wstring(GetLastError()));
        return;
    }

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
#if defined(_M_X64)
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
#else
    DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp;
    frame.AddrStack.Mode = AddrModeFlat;
#endif

    appendFileLogLine(L"Crash stack:");
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machine, proc, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        const DWORD64 addr = frame.AddrPC.Offset;
        if (addr == 0) break;

        DWORD64 disp = 0;
        char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = MAX_SYM_NAME;

        std::wstring lineInfo;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
            lineInfo = std::wstring(L" ") + std::wstring(line.FileName, line.FileName + std::strlen(line.FileName)) +
                       L":" + std::to_wstring(line.LineNumber);
        }

        std::wstring modName = L"?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &hm) && hm) {
            wchar_t buf[MAX_PATH]{};
            if (GetModuleFileNameW(hm, buf, MAX_PATH) > 0) modName = buf;
        }

        if (SymFromAddr(proc, addr, &disp, sym)) {
            appendFileLogLine(L"  " + formatHex(addr) + L" " + modName + L" " + widenUtf8(sym->Name) + L"+" + formatHex(disp) + lineInfo);
        } else {
            appendFileLogLine(L"  " + formatHex(addr) + L" " + modName + lineInfo);
        }
    }
    SymCleanup(proc);
}

bool writeCrashDump(EXCEPTION_POINTERS* ep, const wchar_t* reason) {
    if (g_crashDumpPath.empty()) {
        g_crashDumpPath = joinPath(getExeDir(), L"ton-tunnel-crash.dmp");
    }
    HANDLE hFile = CreateFileW(g_crashDumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        appendFileLogLine(std::wstring(L"Crash dump create failed: ") + g_crashDumpPath + L" gle=" + std::to_wstring(GetLastError()));
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    const auto dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType,
                                     ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(hFile);

    const DWORD gle = ok ? 0 : GetLastError();
    appendFileLogLine(std::wstring(ok ? L"Crash dump written: " : L"Crash dump write failed: ") +
                      g_crashDumpPath + L" reason=" + (reason ? reason : L"unknown") +
                      (ok ? L"" : (L" gle=" + std::to_wstring(gle))));
    logExceptionStack(ep);
    return ok == TRUE;
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    restoreSystemProxy();
    writeCrashDump(ep, L"unhandled-exception");
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() {
    restoreSystemProxy();
    writeCrashDump(nullptr, L"std-terminate");
    std::abort();
}

std::string extractVpsIpFromEgress(const std::string& egress) {
    // Формат: key@IP:PORT
    size_t atPos = egress.find('@');
    if (atPos == std::string::npos) return "";
    size_t colonPos = egress.find(':', atPos);
    if (colonPos == std::string::npos) return "";
    return egress.substr(atPos + 1, colonPos - atPos - 1);
}

void centerControls(HWND hwnd) {
    if (!g_startButton || !g_stopButton || !g_statusLabel || !g_logBox) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int centerX = (rc.right - rc.left) / 2;
    const int centerY = (rc.bottom - rc.top) / 2;
    MoveWindow(g_startButton, centerX - kButtonWidth - 10, centerY - kButtonHeight / 2, kButtonWidth, kButtonHeight, TRUE);
    MoveWindow(g_stopButton, centerX + 10, centerY - kButtonHeight / 2, kButtonWidth, kButtonHeight, TRUE);
    MoveWindow(g_statusLabel, 30, centerY + 70, (rc.right - rc.left) - 60, 48, TRUE);
    MoveWindow(g_logBox, 30, centerY + 125, (rc.right - rc.left) - 60, (rc.bottom - rc.top) - (centerY + 145), TRUE);
}

void setStatusText(const std::wstring& text) {
    if (g_statusLabel) SetWindowTextW(g_statusLabel, text.c_str());
}

void appendLogLine(const std::wstring& line) {
    if (!g_logBox) return;
    const int len = GetWindowTextLengthW(g_logBox);
    SendMessageW(g_logBox, EM_SETSEL, len, len);
    std::wstring text = line + L"\r\n";
    SendMessageW(g_logBox, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    appendFileLogLine(line);
}

bool captureSystemProxyState() {
    INTERNET_PROXY_INFO info{};
    DWORD len = sizeof(info);
    if (!InternetQueryOptionW(nullptr, INTERNET_OPTION_PROXY, &info, &len)) return false;
    g_prevProxy.accessType = info.dwAccessType;
    g_prevProxy.proxy = info.lpszProxy ? info.lpszProxy : L"";
    g_prevProxy.bypass = info.lpszProxyBypass ? info.lpszProxyBypass : L"";
    g_prevProxy.captured = true;
    if (!g_proxyStatePath.empty()) {
        std::ofstream out(g_proxyStatePath, std::ios::binary | std::ios::trunc);
        if (out) {
            out << static_cast<unsigned long>(g_prevProxy.accessType) << "\n";
            out << narrowUtf8(g_prevProxy.proxy) << "\n";
            out << narrowUtf8(g_prevProxy.bypass) << "\n";
        }
    }
    return true;
}

bool loadProxyStateFromDisk(ProxyState& outState) {
    if (g_proxyStatePath.empty()) return false;
    std::ifstream in(g_proxyStatePath, std::ios::binary);
    if (!in) return false;
    std::string line0, line1, line2;
    if (!std::getline(in, line0)) return false;
    if (!std::getline(in, line1)) line1.clear();
    if (!std::getline(in, line2)) line2.clear();
    try {
        outState.accessType = static_cast<DWORD>(std::stoul(line0));
    } catch (...) {
        return false;
    }
    outState.proxy = widenUtf8(line1.c_str());
    outState.bypass = widenUtf8(line2.c_str());
    outState.captured = true;
    return true;
}

void deleteProxyStateFile() {
    if (!g_proxyStatePath.empty()) DeleteFileW(g_proxyStatePath.c_str());
}

void recoverStaleTonProxyIfNeeded() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    DWORD enable = 0;
    DWORD type = REG_DWORD;
    DWORD size = sizeof(enable);
    RegQueryValueExW(hKey, L"ProxyEnable", nullptr, &type, reinterpret_cast<LPBYTE>(&enable), &size);
    wchar_t proxyBuf[1024]{};
    type = REG_SZ;
    size = sizeof(proxyBuf);
    std::wstring proxyServer;
    if (RegQueryValueExW(hKey, L"ProxyServer", nullptr, &type, reinterpret_cast<LPBYTE>(proxyBuf), &size) == ERROR_SUCCESS) {
        proxyServer = proxyBuf;
    }
    const bool staleTonProxy = (enable == 1 && proxyServer.find(L"127.0.0.1:1080") != std::wstring::npos);
    if (!staleTonProxy) {
        RegCloseKey(hKey);
        return;
    }

    ProxyState diskState;
    if (loadProxyStateFromDisk(diskState)) {
        if (diskState.accessType == INTERNET_OPEN_TYPE_PROXY && !diskState.proxy.empty()) {
            RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, reinterpret_cast<const BYTE*>(diskState.proxy.c_str()), static_cast<DWORD>((diskState.proxy.size() + 1) * sizeof(wchar_t)));
            RegSetValueExW(hKey, L"ProxyOverride", 0, REG_SZ, reinterpret_cast<const BYTE*>(diskState.bypass.c_str()), static_cast<DWORD>((diskState.bypass.size() + 1) * sizeof(wchar_t)));
            DWORD on = 1;
            RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&on), sizeof(on));
        } else {
            DWORD off = 0;
            RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&off), sizeof(off));
        }
    } else {
        // Restore connectivity when stale localhost proxy remains after crash.
        DWORD off = 0;
        RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&off), sizeof(off));
    }
    RegCloseKey(hKey);
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    deleteProxyStateFile();
}

bool applySystemSocksProxy(uint16_t port, std::wstring& err) {
    if (!g_prevProxy.captured) captureSystemProxyState();
    std::wstringstream ss;
    ss << L"socks=127.0.0.1:" << port;
    const std::wstring proxy = ss.str();
    const std::wstring bypass = L"<local>";

    HKEY hKey;
    LONG ret = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_SET_VALUE, &hKey);
    if (ret != ERROR_SUCCESS) { err = L"RegOpenKeyEx failed"; return false; }
    DWORD enable = 1;
    RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enable), sizeof(enable));
    RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, reinterpret_cast<const BYTE*>(proxy.c_str()), static_cast<DWORD>((proxy.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(hKey, L"ProxyOverride", 0, REG_SZ, reinterpret_cast<const BYTE*>(bypass.c_str()), static_cast<DWORD>((bypass.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);

    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    DWORD notify = 1;
    InternetSetOptionW(nullptr, INTERNET_OPTION_PROXY_SETTINGS_CHANGED, &notify, sizeof(notify));
    return true;
}

void restoreSystemProxy() {
    if (!g_prevProxy.captured) return;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;
    if (g_prevProxy.accessType == INTERNET_OPEN_TYPE_PROXY && !g_prevProxy.proxy.empty()) {
        RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, reinterpret_cast<const BYTE*>(g_prevProxy.proxy.c_str()), static_cast<DWORD>((g_prevProxy.proxy.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"ProxyOverride", 0, REG_SZ, reinterpret_cast<const BYTE*>(g_prevProxy.bypass.c_str()), static_cast<DWORD>((g_prevProxy.bypass.size() + 1) * sizeof(wchar_t)));
        DWORD enable = 1;
        RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enable), sizeof(enable));
    } else {
        DWORD enable = 0;
        RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enable), sizeof(enable));
    }
    RegCloseKey(hKey);
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    deleteProxyStateFile();
}

bool loadRuntimeConfig(RuntimeConfig& cfg, std::wstring& err) {
    const std::wstring exeDir = getExeDir();
    const std::wstring exeCfg = joinPath(exeDir, L"adnl-transport-config.json");
    const std::wstring cwdCfg = L"adnl-transport-config.json";
    const std::wstring androidRaw = joinPath(exeDir, L"..\\..\\..\\app\\src\\main\\res\\raw\\adnl_transport_config.json");
    bool loadedFromAndroidFallback = false;

    auto loadFromJson = [&](const std::string& json) {
        if (cfg.egress.empty()) cfg.egress = extractJsonStringField(json, "egress_adnl_address");
        if (cfg.clientKey.empty()) cfg.clientKey = extractJsonStringField(json, "client_private_key");
        if (cfg.advertiseHost.empty()) cfg.advertiseHost = extractJsonStringField(json, "local_udp_advertise_host");
        if (cfg.authToken.empty()) cfg.authToken = extractJsonStringField(json, "auth_token");
        if (cfg.socksPort == 1080) {
            const std::string socksPortStr = extractJsonStringField(json, "socks5_port");
            if (!socksPortStr.empty()) {
                const int p = std::atoi(socksPortStr.c_str());
                if (p > 0 && p <= 65535) cfg.socksPort = static_cast<uint16_t>(p);
            }
        }
    };

    { const std::string json = readTextFileUtf8(exeCfg); if (!json.empty()) { loadFromJson(json); appendLogLine(L"Config: loaded " + exeCfg); } }
    if (cfg.egress.empty() || cfg.clientKey.empty() || cfg.authToken.empty()) {
        const std::string json = readTextFileUtf8(cwdCfg);
        if (!json.empty()) { loadFromJson(json); appendLogLine(L"Config: merged from CWD adnl-transport-config.json"); }
    }
    if (cfg.egress.empty() || cfg.clientKey.empty() || cfg.authToken.empty()) {
        const std::string json = readTextFileUtf8(androidRaw);
        if (!json.empty()) { loadFromJson(json); loadedFromAndroidFallback = true; appendLogLine(L"Config: merged Android raw fallback"); }
    }

    if (cfg.egress.empty() || cfg.clientKey.empty()) {
        err = L"Config missing egress_adnl_address/client_private_key.\nPut adnl-transport-config.json near TON-Tunnel.exe";
        return false;
    }

    if (loadedFromAndroidFallback) {
        std::ofstream out(exeCfg, std::ios::binary | std::ios::trunc);
        if (out) {
            out << "{\n"
                << "  \"egress_adnl_address\": \"" << cfg.egress << "\",\n"
                << "  \"local_udp_advertise_host\": \"" << cfg.advertiseHost << "\",\n"
                << "  \"client_private_key\": \"" << cfg.clientKey << "\",\n"
                << "  \"auth_token\": \"" << cfg.authToken << "\",\n"
                << "  \"socks5_port\": \"" << cfg.socksPort << "\"\n"
                << "}\n";
            appendLogLine(L"Config: wrote fallback config near exe");
        }
    }
    return true;
}

// ADNL inbound message callback (called from TON actor thread)
void __cdecl onAdnlMessage(const uint8_t* data, size_t len, void* /*user*/) {
    if (!data || len == 0) return;
    std::vector<uint8_t> packet(data, data + len);
    {
        std::lock_guard<std::mutex> lock(g_adnlMutex);
        g_adnlInboundQueue.push_back(std::move(packet));
    }
    g_adnlCv.notify_one();
}

// ITransportBackend implementation that calls ton_adnl_client_* directly
class DirectAdnlBackend final : public ITransportBackend {
public:
    DirectAdnlBackend() = default;
    ~DirectAdnlBackend() override { stop(); }

    bool start(const std::string& key, const std::string& egress, const std::string& advertise, std::string& err) {
        g_adnlHandle = ton_adnl_client_create(key.c_str(), egress.c_str(), advertise.c_str());
        if (!g_adnlHandle) { err = "ton_adnl_client_create returned null"; return false; }
        ton_adnl_client_set_on_message(g_adnlHandle, onAdnlMessage, nullptr);
        if (ton_adnl_client_start(g_adnlHandle) != 0) {
            ton_adnl_client_destroy(g_adnlHandle);
            g_adnlHandle = nullptr;
            err = "ton_adnl_client_start failed";
            return false;
        }
        g_adnlStarted.store(true);
        return true;
    }

    void stop() {
        g_adnlStarted.store(false);
        {
            std::lock_guard<std::mutex> lock(g_adnlMutex);
            g_adnlInboundQueue.clear();
        }
        g_adnlCv.notify_all();
        if (g_adnlHandle) {
            ton_adnl_client_stop(g_adnlHandle);
            ton_adnl_client_destroy(g_adnlHandle);
            g_adnlHandle = nullptr;
        }
    }

    int sendPacket(const std::vector<uint8_t>& packet) override {
        if (!g_adnlStarted.load() || !g_adnlHandle) return -1;
        return ton_adnl_client_send(g_adnlHandle, packet.data(), packet.size());
    }

    bool receivePacket(std::vector<uint8_t>& outPacket, int timeoutMs) override {
        std::unique_lock<std::mutex> lock(g_adnlMutex);
        if (!g_adnlCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                [&]{ return !g_adnlInboundQueue.empty() || !g_adnlStarted.load(); })) {
            return false;
        }
        if (g_adnlInboundQueue.empty()) return false;
        outPacket = std::move(g_adnlInboundQueue.front());
        g_adnlInboundQueue.pop_front();
        return true;
    }

private:
    std::mutex mu_;
};

void stopCore() {
    restoreSystemProxy();
    if (g_bridge) { g_bridge->stop(); g_bridge.reset(); }
    if (g_socks5) { g_socks5->stop(); g_socks5.reset(); }
    if (g_core) { g_core->stop(); g_core.reset(); }
    // Stop ADNL backend
    g_adnlStarted.store(false);
    {
        std::lock_guard<std::mutex> lock(g_adnlMutex);
        g_adnlInboundQueue.clear();
    }
    g_adnlCv.notify_all();
    if (g_adnlHandle) {
        ton_adnl_client_stop(g_adnlHandle);
        ton_adnl_client_destroy(g_adnlHandle);
        g_adnlHandle = nullptr;
    }
    if (g_startButton) EnableWindow(g_startButton, TRUE);
    if (g_stopButton) EnableWindow(g_stopButton, FALSE);
    setStatusText(L"Status: stopped (system proxy restored)");
}

void startTunnel(HWND hwnd) {
    stopCore();
    appendLogLine(L"START pressed");

    RuntimeConfig cfgFile{};
    std::wstring cfgErr;
    if (!loadRuntimeConfig(cfgFile, cfgErr)) {
        appendLogLine(L"Config error: " + cfgErr);
        MessageBoxW(hwnd, cfgErr.c_str(), L"TON-Tunnel", MB_OK | MB_ICONERROR);
        return;
    }

    // ── 1. Запускаем ADNL backend напрямую ──
    appendLogLine(L"Starting ADNL backend...");
    {
        std::string adnlErr;
        auto backend = std::make_shared<DirectAdnlBackend>();
        if (!backend->start(cfgFile.clientKey, cfgFile.egress, cfgFile.advertiseHost, adnlErr)) {
            std::wstring msg = L"ADNL start failed: ";
            msg += std::wstring(adnlErr.begin(), adnlErr.end());
            appendLogLine(msg);
            setStatusText(msg);
            MessageBoxW(hwnd, msg.c_str(), L"TON-Tunnel", MB_OK | MB_ICONERROR);
            stopCore();
            return;
        }
        appendLogLine(L"ADNL backend started");

        g_core = std::make_shared<TonClientCore>(std::move(backend));
        if (!g_core->start()) {
            appendLogLine(L"Core start failed");
            MessageBoxW(hwnd, L"Failed to start core runtime", L"TON-Tunnel", MB_OK | MB_ICONERROR);
            stopCore();
            return;
        }
        appendLogLine(L"Core started");
    }

    // ── 2. Запускаем SOCKS5 сервер ──
    {
        std::string socksErr;
        g_socks5 = std::make_unique<LocalSocks5Server>(
            g_core, cfgFile.authToken,
            [](const std::string& msg) {
                std::wstring w(msg.begin(), msg.end());
                appendFileLogLine(w);
                if (g_logBox) PostMessageW(g_logBox, kMsgAppendLog, 0, reinterpret_cast<LPARAM>(new std::wstring(w)));
            }
        );
        if (!g_socks5->start(cfgFile.socksPort, socksErr)) {
            std::wstring msg = L"SOCKS5 start failed: ";
            msg += std::wstring(socksErr.begin(), socksErr.end());
            appendLogLine(msg);
            MessageBoxW(hwnd, msg.c_str(), L"TON-Tunnel", MB_OK | MB_ICONERROR);
            stopCore();
            return;
        }
        appendLogLine(L"SOCKS5 started on 127.0.0.1:" + std::to_wstring(cfgFile.socksPort));
    }

    // ── 3. Устанавливаем системный SOCKS5 прокси ──
    {
        std::wstring proxyErr;
        if (applySystemSocksProxy(cfgFile.socksPort, proxyErr)) {
            appendLogLine(L"System proxy set to 127.0.0.1:" + std::to_wstring(cfgFile.socksPort));
        } else {
            appendLogLine(L"System proxy failed: " + proxyErr);
        }
    }

    // ── 4. Запускаем TUN через wintun + lwIP bridge ──
    {
        g_bridge = std::make_unique<WintunLwipBridge>();
        std::string bridgeErr;
        if (g_bridge->start("127.0.0.1", cfgFile.socksPort,
                [](const std::string& msg) { /* logger */ }, bridgeErr))
        {
            appendLogLine(L"TUN bridge started");
            appendLogLine(L"TUN bridge started - routes configured by system proxy");
        } else {
            appendLogLine(L"TUN bridge start failed: " + std::wstring(bridgeErr.begin(), bridgeErr.end()));
        }
    }

    std::wstringstream ss;
    ss << L"Status: running | ADNL active | SOCKS5 127.0.0.1:" << cfgFile.socksPort;
    setStatusText(ss.str());
    EnableWindow(g_startButton, FALSE);
    EnableWindow(g_stopButton, TRUE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_logFilePath = joinPath(getExeDir(), L"ton-tunnel.log");
        g_proxyStatePath = joinPath(getExeDir(), L"ton-proxy-state.txt");
        recoverStaleTonProxyIfNeeded();
        g_startButton = CreateWindowW(L"BUTTON", L"START", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            0, 0, kButtonWidth, kButtonHeight, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonIdStart)), GetModuleHandleW(nullptr), nullptr);
        g_stopButton = CreateWindowW(L"BUTTON", L"STOP", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            0, 0, kButtonWidth, kButtonHeight, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonIdStop)), GetModuleHandleW(nullptr), nullptr);
        g_statusLabel = CreateWindowW(L"STATIC", L"Status: idle", WS_VISIBLE | WS_CHILD | SS_LEFT,
            0, 0, 200, 40, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLabelIdStatus)), GetModuleHandleW(nullptr), nullptr);
        g_logBox = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditIdLog)), GetModuleHandleW(nullptr), nullptr);
        EnableWindow(g_stopButton, FALSE);
        centerControls(hwnd);
        appendLogLine(L"App started");
        appendLogLine(L"Log file: " + g_logFilePath);
        PostMessageW(hwnd, kMsgAutoStart, 0, 0);
        return 0;
    }
    case kMsgAppendLog: {
        auto* w = reinterpret_cast<std::wstring*>(lParam);
        if (w) { appendLogLine(*w); delete w; }
        return 0;
    }
    case WM_SIZE: centerControls(hwnd); return 0;
    case kMsgAutoStart: startTunnel(hwnd); return 0;
    case WM_COMMAND: {
        const int controlId = LOWORD(wParam);
        if (controlId == kButtonIdStart) { startTunnel(hwnd); return 0; }
        if (controlId == kButtonIdStop) { stopCore(); return 0; }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_DESTROY: stopCore(); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, L"Global\\TON_Tunnel_SingleInstance");
    if (!singleInstance) {
        return 10;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"TON-Tunnel is already running.", L"TON-Tunnel", MB_OK | MB_ICONINFORMATION);
        CloseHandle(singleInstance);
        return 0;
    }

    AllocConsole();
    freopen("CONOUT$", "w", stderr);
    freopen("CONOUT$", "w", stdout);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    std::set_terminate(onTerminate);
    const wchar_t* kClassName = L"TonTunnelMainWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kClassName, L"TON-Tunnel", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth, kWindowHeight, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 2;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CloseHandle(singleInstance);
    return static_cast<int>(msg.wParam);
}
