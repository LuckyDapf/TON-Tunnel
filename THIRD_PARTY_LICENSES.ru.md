# Стороннее ПО (third-party)

Код из внешних проектов, лежащий в этом репозитории (`ton-egress-node/third_party/`, `app/src/main/cpp/third_party/` и т. д.), распространяется **не** под единственной AGPL проекта — у каждого дерева сохранена **исходная** лицензия производителей (файлы `LICENSE*`, `NOTICE`, `AUTHORS` в подпапках).

Краткая карта ключевых компонентов (зеркально к [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md)):

| Компонент | Обычный путь |
|-----------|----------------|
| Стек TON (адаптированная ветка) | `ton-egress-node/third_party/ton/` |
| OpenSSL | `.../third-party/openssl/` |
| libsodium | `.../third-party/sodium/` |
| CRC32c | `.../third-party/crc32c/` |
| zlib | `.../third-party/zlib/` |
| badvpn + lwIP (VPN на Android JNI) | `app/src/main/cpp/third_party/badvpn/` |
| badvpn + lwIP (ветка сборки под Windows GUI) | `TON-Tunnel/third_party/badvpn/` |

При распространении релиза **сохраняйте** все upstream-лицензии и уведомления об авторских правах. Полный англоязычный обзор: [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
