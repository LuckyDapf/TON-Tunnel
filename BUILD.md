# BUILD.md

Build instructions for:

- Android client (`app/`)
- Windows desktop C++ client (`TON-Tunnel/`)
- Linux egress node (`ton-egress-node/`)

This document is written for developers who need deterministic local builds.

## 1. Requirements

## 1.1 Android Client Toolchain

- JDK 17 (recommended for recent Android Gradle Plugin workflows)
- Android Studio (latest stable) or command-line Android SDK tools
- Android SDK Platform + Build Tools required by the project
- Android NDK (project currently references NDK 28.x in generated build artifacts)
- CMake (via Android SDK)
- Gradle Wrapper (already included: `gradlew`, `gradlew.bat`)

## 1.2 Server Toolchain (Linux)

- GCC or Clang with C++20 support
- CMake >= 3.16
- Ninja or Make
- `git` with submodule support
- Common build dependencies (Ubuntu/Debian):

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config libssl-dev zlib1g-dev
```

## 1.3 Source Checkout

From repository root:

```bash
git submodule update --init --recursive
```

This step is mandatory because TON third-party sources are used by both Android and server-side ADNL builds.

## 2. Android Build

## 2.1 Quick Build (Debug APK)

From repository root:

```bash
./gradlew :app:assembleDebug
```

Windows:

```powershell
.\gradlew.bat :app:assembleDebug
```

Output APK:

- `app/build/outputs/apk/debug/app-debug.apk`

## 2.2 Native Components Built for Android

Main CMake entry:

- `app/src/main/cpp/CMakeLists.txt`

Targets of interest:

- `ton_transport` (JNI transport bridge)
- `ton_adnl_client` (ADNL backend for Android, when enabled)
- `tun2socks_jni` (tun2socks JNI bridge)

Important behavior:

- By default, Android build path is configured to prefer source-based ADNL client build.
- Built `libton_adnl_client.so` is copied into `app/src/main/jniLibs/<ABI>/`.

## 2.3 Android Build Troubleshooting

- **NDK/CMake mismatch**: ensure Android Studio SDK Manager has required NDK and CMake versions.
- **Stale CMake cache**: clean Gradle + `.cxx` intermediates:

```bash
./gradlew clean
```

Then rebuild.

- **JNI symbol/runtime mismatch**: verify that updated native `.so` files were copied to `app/src/main/jniLibs/<ABI>/`.

## 3. Windows Desktop Client (`TON-Tunnel/`)

CMake project `ton_tunnel_windows` — C++20 клиент с Win32 UI, Wintun ↔ lwIP ↔ tun2socks, локальный SOCKS5 и `ton_adnl_client_backend` (тот же модуль, что и на Android, собирается из `app/src/main/cpp/ton_adnl_client_backend.cpp`). TON SDK подключается из `../ton-egress-node/third_party/ton`.

**Requirements (typical MSVC flow):**

- Visual Studio 2022 (C++ desktop workload)
- CMake ≥ 3.22
- Strawberry Perl (CMakeLists pins `PERL_EXECUTABLE` / `pkg-config` shim — при необходимости поправьте пути под вашу установку)
- Wintun runtime / driver as required by your deployment (see `PROJECT_OVERVIEW_RU.md` § Windows)

**Configure & build** (from repo root, out-of-tree):

```powershell
cmake -S TON-Tunnel -B TON-Tunnel/build -G "Visual Studio 17 2022" -A x64
cmake --build TON-Tunnel/build --config Release
```

MinGW / Ninja variants are possible if toolchain paths in `TON-Tunnel/CMakeLists.txt` are adapted (project was primarily tuned for MSVC).

For architecture and binary layout, see **`PROJECT_OVERVIEW_RU.md`** (section *Windows Desktop клиент*).

## 4. Server Build (Egress Node)

Server project root:

- `ton-egress-node/`

## 4.1 Build Stub Transport Binary (default)

From `ton-egress-node/`:

```bash
cmake -S . -B build -G Ninja
cmake --build build -j
```

This builds:

- `ton_egress_node`

The default mode includes stub transport sources unless ADNL backend mode is explicitly enabled.

## 4.2 Build with Real ADNL Backend

From `ton-egress-node/`:

```bash
cmake -S . -B build-adnl -G Ninja -DTON_EGRESS_BUILD_ADNL_BACKEND=ON
cmake --build build-adnl -j
```

Expected artifacts include:

- `ton_egress_node`
- `libton_adnl_backend.so`

Relevant options in `ton-egress-node/CMakeLists.txt`:

- `TON_EGRESS_BUILD_ADNL_BACKEND`
- `TON_DISABLE_QUIC_FOR_EGRESS`
- `TON_DISABLE_LIBBACKTRACE_FOR_EGRESS`

## 4.3 Server Run

Minimal run:

```bash
./ton_egress_node --config ./config.json
```

Config file:

- `ton-egress-node/config.json`

## 4.4 Server Build Troubleshooting

- **Missing third-party static libs**: indicates incomplete or inconsistent dependency build tree.
  - Re-run submodule initialization.
  - Use a fresh build directory.
- **OpenSSL/Zlib detection issues**:
  - Ensure system dev packages are installed.
  - Remove stale CMake cache and reconfigure.

## 5. Reproducible Build Recommendations

- Use separate build directories per mode (`build`, `build-adnl`, Android `.cxx`).
- Do not mix toolchains in one CMake cache.
- Pin JDK/NDK versions in CI.
- Always execute `git submodule update --init --recursive` in clean environments.

## 6. CI Suggestions

- Android lane:
  - `./gradlew :app:assembleDebug`
- Server lane:
  - `cmake -S ton-egress-node -B ton-egress-node/build-adnl -G Ninja -DTON_EGRESS_BUILD_ADNL_BACKEND=ON`
  - `cmake --build ton-egress-node/build-adnl -j`
- Optional Windows lane (when a Windows runner + VS is available):
  - `cmake -S TON-Tunnel -B TON-Tunnel/build -G "Visual Studio 17 2022" -A x64`
  - `cmake --build TON-Tunnel/build --config Release`

Cache:

- Gradle cache
- Android SDK/NDK cache
- CMake/Ninja intermediate artifacts per target platform

