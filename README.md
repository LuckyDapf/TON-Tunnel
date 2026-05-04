# TON Tunnel

*[Русское описание: `README.ru.md`](README.ru.md)*

TON Tunnel is a next-generation VPN platform that provides secure internet traffic transmission through its own high-performance transport layer based on ADNL (TON ecosystem).

The solution implements system traffic capture through a virtual network interface (TUN), routes it through a local proxy layer, and then transmits it to a remote server (egress node) that performs internet egress.

ADNL is used as a low-level transport mechanism for secure stream data transmission between client and server. On top of it, we implement our own HOPE streaming protocol that provides connection multiplexing and data flow control.

🚀 **Key Product Value**
- Alternative VPN transport using UDP-oriented architecture
- Enhanced resistance to filtering and signature analysis through non-standard transport layer
- Modular architecture: TUN → SOCKS5 → stream protocol → ADNL → egress
- Cross-platform (Android / Windows / Linux server)
- Scalable server component (egress nodes)

⚙️ **Architecture in One Paragraph**

User traffic is intercepted through a virtual network interface, converted to stream connections through a SOCKS5 layer, and transmitted to the server via the custom HOPE protocol operating on top of ADNL. The server component performs stream decoding and provides standard TCP/UDP internet egress through NAT.

📌 **Important (Positioning)**

TON Tunnel does not use the TON network as routing infrastructure, but applies ADNL as a secure transport layer for data delivery between client and server.

---

ADNL-based tunneling stack for Android (Kotlin/NDK), **Windows desktop** (`TON-Tunnel/`, C++20 / Win32 / Wintun), and Linux egress nodes.

The project implements a custom stream-multiplexed transport (`OPEN/DATA/CLOSE/ERROR`) on top of TON ADNL.  
Android uses a local SOCKS5 endpoint inside a VPN service; Windows uses its own SOCKS5 + Wintun path; the egress server resolves destination hosts, opens TCP sockets, and relays payload frames bidirectionally.

## Repository Layout

- `app/` — Android client (Kotlin + JNI + NDK).
- `TON-Tunnel/` — **Desktop C++ client** (Windows: Win32 GUI, Wintun, local SOCKS5, ADNL via shared TON stack; see `TON-Tunnel/CMakeLists.txt`).
- `ton-egress-node/` — Linux server (C++), optional ADNL backend shared library (`libton_adnl_backend.so`).
- `PROJECT_OVERVIEW_RU.md` — project background and operational notes (Russian).
- Russian-language VPS guide: [`ton-egress-node/README.md`](ton-egress-node/README.md).

## Key Components

- **Android transport runtime**
  - `app/src/main/java/com/example/dapf/tongate/data/native/NativeTonTransport.kt`
  - Stream lifecycle, frame encoding/decoding, segmentation, counters.
- **Android SOCKS/VPN bridge**
  - `app/src/main/java/com/example/dapf/tongate/data/vpn/LocalSocks5Server.kt`
  - SOCKS handshake, stream open/close mapping, upstream/downstream piping.
- **Native ADNL client backend**
  - `app/src/main/cpp/ton_adnl_client_backend.cpp`
  - TON ADNL integration for Android.
- **Desktop C++ transport (Windows)**
  - `TON-Tunnel/TonProtocol.cpp`, `TON-Tunnel/TonClientCore.cpp`, `TON-Tunnel/LocalSocks5Server.cpp`, `TON-Tunnel/WintunLwipBridge.cpp`, `TON-Tunnel/TON-Tunnel.cpp`
  - Reuses the same ADNL client backend sources and `ton-egress-node/third_party/ton` CMake subtree (see `TON-Tunnel/CMakeLists.txt`).
- **Egress node core**
  - `ton-egress-node/src/egress_node.cpp`
  - OPEN validation, DNS+TCP connect, stream map, relay loop, close semantics.
- **Protocol codec (server side)**
  - `ton-egress-node/include/protocol.hpp`
  - `ton-egress-node/src/protocol.cpp`

## Protocol Summary

The project uses a custom binary frame protocol over ADNL payloads:

- `OPEN (1)` — request stream open (`stream_id`, `host`, `port`, `token`)
- `DATA (2)` — payload chunk (`stream_id`, `payload_len`, bytes)
- `CLOSE (3)` — stream close (`stream_id`)
- `ERROR (4)` — stream error (`stream_id`, string message)

Full protocol specification is provided in `PROTOCOL.md`.

## Build

See `BUILD.md` for full build instructions:

- Android (Gradle + Android NDK + CMake)
- Windows desktop C++ client (`TON-Tunnel/` — CMake + MSVC, Wintun; see BUILD.md § 3)
- Linux egress node (CMake)
- Optional ADNL backend build mode (`libton_adnl_backend.so`)

## Runtime Notes

- Current server config is in `ton-egress-node/config.json`.
- ADNL backend verbosity is intentionally constrained to reduce diagnostics noise.
- Stability counters are emitted periodically on both client and server.

## Security and Scope

This is a controlled tunnel system, not a generic public proxy service.  
Deployment must include strict key/token handling, host/port policy, and production hardening at OS/network level.

See [`SECURITY.md`](SECURITY.md) (English) and [`SECURITY.ru.md`](SECURITY.ru.md) (Russian companion).

## Community / Документы сообщества

| Topic | EN | RU |
|-------|-----|-----|
| Contributing | [`CONTRIBUTING.md`](CONTRIBUTING.md) | [`CONTRIBUTING.ru.md`](CONTRIBUTING.ru.md) |
| Security | [`SECURITY.md`](SECURITY.md) | [`SECURITY.ru.md`](SECURITY.ru.md) |
| Code of Conduct | [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) (Contributor Covenant 2.1) | [`CODE_OF_CONDUCT.ru.md`](CODE_OF_CONDUCT.ru.md) |
| Documentation index | [.github/DOCUMENTATION.md](.github/DOCUMENTATION.md) | same |
| GitHub **About** field (copy-paste) | [.github/GITHUB_ABOUT.md](.github/GITHUB_ABOUT.md) | [.github/GITHUB_ABOUT.ru.md](.github/GITHUB_ABOUT.ru.md) |

Russian overview: [`README.ru.md`](README.ru.md).

## License

- Original work in this repository: **GNU Affero General Public License v3.0** — see [`LICENSE`](LICENSE).
- Informal Russian translation of the AGPL (not legally substitute for the English text): [`LICENSE.ru`](LICENSE.ru).
- Project copyright summary: [`NOTICE`](NOTICE).

Bundled upstream and third-party libraries (TON tree, OpenSSL, libsodium, etc.) retain their **own** licenses — see [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) · [`THIRD_PARTY_LICENSES.ru.md`](THIRD_PARTY_LICENSES.ru.md).

**SPDX:** `AGPL-3.0-or-later` (for project-authored files outside vendored subtrees).

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) · [`CONTRIBUTING.ru.md`](CONTRIBUTING.ru.md) · Pull request template in [`.github/pull_request_template.md`](.github/pull_request_template.md).