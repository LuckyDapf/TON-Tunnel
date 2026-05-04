# TON Tunnel

*English readme:* [`README.md`](README.md)

TON Tunnel — это VPN-платформа нового поколения, обеспечивающая защищённую передачу интернет-трафика через собственный высокопроизводительный транспортный слой на базе ADNL (экосистема TON).

Решение реализует захват системного трафика через виртуальный сетевой интерфейс (TUN), его маршрутизацию через локальный прокси-слой и последующую передачу на удалённый сервер (egress-узел), который выполняет выход в интернет.

ADNL используется как низкоуровневый транспортный механизм для защищённой передачи потоковых данных между клиентом и сервером. Поверх него реализован собственный потоковый протокол HOPE, обеспечивающий мультиплексирование соединений и контроль потоков данных.

🚀 **Ключевая ценность продукта**
- Альтернативный транспорт VPN-трафика с использованием UDP-ориентированной архитектуры
- Повышенная устойчивость к фильтрации и сигнатурному анализу за счёт нестандартного транспортного слоя
- Модульная архитектура: TUN → SOCKS5 → stream protocol → ADNL → egress
- Кроссплатформенность (Android / Windows / Linux сервер)
- Масштабируемая серверная часть (egress nodes)

⚙️ **Архитектура в одном абзаце**

Пользовательский трафик перехватывается через виртуальный сетевой интерфейс, преобразуется в потоковые соединения через SOCKS5-слой и передаётся на сервер через кастомный протокол HOPE, работающий поверх ADNL. Серверный компонент выполняет декодирование потоков и обеспечивает стандартный TCP/UDP выход в интернет через NAT.

📌 **Важно (позиционирование)**

TON Tunnel не использует сеть TON как маршрутизирующую инфраструктуру, а применяет ADNL как защищённый транспортный уровень для доставки данных между клиентом и сервером.

---

Система туннелирования на базе ADNL для **Android** (Kotlin/NDK), **Windows** (`TON-Tunnel/`, C++20) и **Linux**‑сервера выхода (egress).

Проект реализует собственный мультиплексированный транспорт (`OPEN/DATA/CLOSE/ERROR`) поверх TON ADNL.  
На Android поднимается локальный SOCKS5 в составе VPN-сервиса; в `TON-Tunnel` — свой локальный SOCKS5 и стек поверх Wintun; сервер egress резолвит хосты, открывает TCP-соединения и ретранслирует фреймы в обе стороны.

## Структура репозитория

- `app/` — Android-клиент (Kotlin + JNI + NDK).
- `TON-Tunnel/` — **настольный C++‑клиент** для Windows (Win32 GUI, Wintun, локальный SOCKS5, ADNL через общее дерево TON SDK; CMake — `TON-Tunnel/CMakeLists.txt`).
- `ton-egress-node/` — серверный узел на Linux (C++), включая опциональный ADNL backend.
- `PROJECT_OVERVIEW_RU.md` — расширенный обзор и заметки по проекту.

## Основные компоненты

- **Android transport runtime**
  - `app/src/main/java/com/example/dapf/tongate/data/native/NativeTonTransport.kt`
  - Жизненный цикл stream, кодирование/декодирование фреймов, сегментация, счетчики.
- **Android SOCKS/VPN bridge**
  - `app/src/main/java/com/example/dapf/tongate/data/vpn/LocalSocks5Server.kt`
  - SOCKS handshake, связывание stream open/close, прокачка в обе стороны.
- **Нативный ADNL client backend**
  - `app/src/main/cpp/ton_adnl_client_backend.cpp`
  - Интеграция Android с TON ADNL.
- **C++‑клиент на Windows (desktop)**
  - `TON-Tunnel/TonProtocol.cpp`, `TON-Tunnel/TonClientCore.cpp`, `TON-Tunnel/LocalSocks5Server.cpp`, `TON-Tunnel/WintunLwipBridge.cpp`, `TON-Tunnel/TON-Tunnel.cpp`
  - Те же исходники ADNL backend и дерево `ton-egress-node/third_party/ton` подключаются через `TON-Tunnel/CMakeLists.txt`.
- **Ядро egress node**
  - `ton-egress-node/src/egress_node.cpp`
  - Валидация OPEN, DNS+TCP connect, таблица stream, relay loop, закрытия.
- **Кодек протокола (server side)**
  - `ton-egress-node/include/protocol.hpp`
  - `ton-egress-node/src/protocol.cpp`

## Кратко о протоколе

В проекте используется собственный бинарный frame-протокол в payload ADNL:

- `OPEN (1)` — открыть stream (`stream_id`, `host`, `port`, `token`)
- `DATA (2)` — блок полезных данных (`stream_id`, `payload_len`, bytes)
- `CLOSE (3)` — закрыть stream (`stream_id`)
- `ERROR (4)` — ошибка stream (`stream_id`, строка)

Полная спецификация — в `PROTOCOL.md`.

## Сборка

Полные инструкции в `BUILD.md`:

- Android (Gradle + Android NDK + CMake)
- Настольный клиент под Windows (`TON-Tunnel/` — CMake + MSVC, Wintun; см. `BUILD.md`, раздел 3)
- Linux egress node (CMake)
- Опциональная сборка ADNL backend (`libton_adnl_backend.so`)

## Эксплуатационные замечания

- Текущая серверная конфигурация: `ton-egress-node/config.json`.
- Verbose-логирование ADNL ограничено для снижения шумовых логов.
- Клиент и сервер публикуют агрегированные счетчики стабильности.

## Безопасность и зона ответственности

Это управляемая туннельная система, а не публичный open proxy.  
Для продакшена обязательны: строгий контроль ключей/токенов, политика портов/адресов и hardening хоста.

Политика безопасности: [`SECURITY.md`](SECURITY.md) (EN) · [`SECURITY.ru.md`](SECURITY.ru.md) (RU).

## Сообщество и справочник

| Тема | English | Русский |
|------|---------|---------|
| Участие в разработке | [`CONTRIBUTING.md`](CONTRIBUTING.md) | [`CONTRIBUTING.ru.md`](CONTRIBUTING.ru.md) |
| Кодекс поведения | [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) | [`CODE_OF_CONDUCT.ru.md`](CODE_OF_CONDUCT.ru.md) |
| Карта документации | [.github/DOCUMENTATION.md](.github/DOCUMENTATION.md) | см. столбцы EN/RU там же |
| Поля **About** на GitHub | [.github/GITHUB_ABOUT.md](.github/GITHUB_ABOUT.md) | [.github/GITHUB_ABOUT.ru.md](.github/GITHUB_ABOUT.ru.md) |

Шаблон pull request: [`.github/pull_request_template.md`](.github/pull_request_template.md).

## Лицензия

Оригинальный код монорепозитория распространяется под **GNU Affero General Public License v3.0** — канонический англоязычный текст: [`LICENSE`](LICENSE).

Неофициальный русскоязычный перевод AGPL (для ознакомления, без замены английского текста в юридическом смысле): [`LICENSE.ru`](LICENSE.ru).

Copyright и краткое пояснение: [`NOTICE`](NOTICE).

Сторонние библиотеки (ветка TON, OpenSSL, libsodium и др.) сохраняют **свои** лицензии — см. [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) · [`THIRD_PARTY_LICENSES.ru.md`](THIRD_PARTY_LICENSES.ru.md).

**SPDX:** `AGPL-3.0-or-later` (для собственных файлов проекта вне каталогов вендоров).

## Контрибьюция

См. [`CONTRIBUTING.ru.md`](CONTRIBUTING.ru.md) или [`CONTRIBUTING.md`](CONTRIBUTING.md).