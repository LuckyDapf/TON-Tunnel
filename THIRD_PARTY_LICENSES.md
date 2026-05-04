# Third-party software

Vendor code bundled in this repository is **not covered** by your choice of SPDX identifier for original project files: each tree keeps its upstream license files.

Major trees most users care about:

| Component | Typical path | Notes |
|-----------|---------------|-------|
| TON stack (adapted upstream) | `ton-egress-node/third_party/ton/` | Many components share TON-era licensing — check subfolders (`LICENSE*` / COPYING). |
| OpenSSL | `ton-egress-node/third_party/ton/third-party/openssl/` | Apache-style license (`LICENSE.txt`). |
| libsodium | `ton-egress-node/third_party/ton/third-party/sodium/` | ISC — see upstream `LICENSE`. |
| CRC32c | `ton-egress-node/third_party/ton/third-party/crc32c/` | BSD 3-Clause. |
| zlib | `ton-egress-node/third_party/ton/third-party/zlib/` | zlib license. |
| LZ4 | (built via CMake, sources under TON CMake flow) | BSD 2-Clause (see upstream). |
| badvpn / lwIP core (VPN stack, Android JNI) | `app/src/main/cpp/third_party/badvpn/` | badvpn GPLv2+, lwIP custom — read each subdirectory. |
| badvpn / lwIP (desktop Windows build tree) | `TON-Tunnel/third_party/badvpn/` | Same upstream licensing as Android copy; shipped separately under `TON-Tunnel/`. |

Android may also consume prebuilt `libton_adnl_client.so`; when you rebuild from CMake, binaries are regenerated under `.cxx/` locally (ignored by `.gitignore`).

**Distribution:** Preserve all upstream `LICENSE*`, copyright headers, and `NOTICE`/`AUTHORS` files when you fork or publish releases.

Russian summary: [`THIRD_PARTY_LICENSES.ru.md`](THIRD_PARTY_LICENSES.ru.md).
