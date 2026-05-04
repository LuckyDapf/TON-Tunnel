# 🛡️ TON Tunnel — Полное Техническое Описание

**Проект:** TON Tunnel  
**Автор:** lucky  
**Протокол:** HOPE (High-performance Overlay Protocol for Egress) v0.1  
**Языки:** C++20 / Kotlin / Java (JNI) / CMake / Gradle  
**Платформы:** Android (основной клиент) + Windows Desktop + VPS Linux (сервер)  

---

## 1. Концепция

TON Tunnel — это **полноценный VPN-туннель**, который отправляет пользовательский трафик через **сеть TON (The Open Network)** по протоколу **ADNL**, а не через классические VPN-протоколы (OpenVPN, WireGuard, Shadowsocks).

Главное архитектурное отличие от обычных прокси/VPN — **трафик идёт внутри ADNL-сообщений**, подписанных Ed25519 ключами, что делает его неотличимым от обычного P2P-трафика сети TON.

```
ТЕЛЕФОН (Android)                        VPS (Linux)
┌─────────────────────┐                 ┌──────────────────────┐
│  Приложение         │                 │   Egress Server      │
│  ┌───────────────┐  │   ADNL/UDP     │   ┌───────────────┐  │
│  │ VpnService    │  │ ═══════════════>│   │ ADNL Backend  │  │
│  │    ↓          │  │  (шифрованный   │   │    ↓          │  │
│  │  TUN          │  │   канал)        │   │  TCP connect  │──>│ Интернет
│  │    ↓          │  │                 │   │    ↓          │  │
│  │  tun2socks    │  │                 │   │  Релей данных │<──│
│  │    ↓          │  │                 │   └───────────────┘  │
│  │  SOCKS5       │  │                 └──────────────────────┘
│  │    ↓          │  │
│  │  HOPE фреймы  │  │
│  │    ↓          │  │
│  │  ADNL клиент  │  │
│  └───────────────┘  │
└─────────────────────┘
```

**Путь трафика:**
1. Весь трафик телефона попадает в виртуальный TUN-интерфейс
2. `tun2socks` (lwIP TCP/IP стек) конвертирует IP-пакеты в SOCKS5-соединения
3. Локальный SOCKS5-сервер оборачивает запросы в HOPE-фреймы
4. HOPE-фреймы уходят через ADNL (Ed25519-шифрованный UDP) на VPS
5. VPS расшифровывает, открывает TCP-соединение в интернет, возвращает ответ

---

## 2. Протокол HOPE (High-performance Overlay Protocol for Egress)

**Версия:** 0.1 (draft)  
**Транспорт:** ADNL payload (raw message channel)  
**Endianness:** network byte order (big-endian)

### 2.1 Формат фрейма

Каждый фрейм начинается с заголовка:

```
| type (u8) | stream_id (u32) | type-specific payload... |
```

### 2.2 Типы фреймов

| Тип | Название | Направление | Назначение |
|-----|----------|-------------|------------|
| 1 | `OPEN` | Client→Server | Открыть TCP-поток |
| 1 | `OPEN ACK` | Server→Client | Подтверждение открытия |
| 2 | `DATA` | ↔ | Полезная нагрузка |
| 3 | `CLOSE` | ↔ | Закрыть поток |
| 4 | `ERROR` | Server→Client | Ошибка потока |

### 2.3 Wire format

**OPEN (Client→Server):**
```
type:         u8   = 1
stream_id:    u32
host_len:     u16
host:         bytes[host_len]    (UTF-8, например "example.com")
port:         u16
token_len:    u16
token:        bytes[token_len]   (auth token)
```

**OPEN ACK (Server→Client):**
```
type:         u8   = 1
stream_id:    u32
```
(без host/port/token — минимальный ACK)

**DATA (↔):**
```
type:         u8   = 2
stream_id:    u32
payload_len:  u32
payload:      bytes[payload_len]
```

**CLOSE (↔):**
```
type:         u8   = 3
stream_id:    u32
```

**ERROR (Server→Client):**
```
type:         u8   = 4
stream_id:    u32
msg_len:      u16
message:      bytes[msg_len]     (UTF-8, диагностика)
```

### 2.4 Машина состояний потока

```
Client                          Server
  |                               |
  |──── OPEN (stream=1) ─────────>|  проверка токена
  |                               |  DNS + connect()
  |<─── OPEN ACK (stream=1) ──────|  поток открыт
  |                               |
  |<─── DATA (stream=1) ──>───────|  релей данных ↔
  |                               |
  |<─── CLOSE (stream=1) ────────>|  закрытие
  |                               |
```

### 2.5 Ключевые особенности протокола

- **No ACK/SEQ numbers** — протокол не реализует собственную гарантию доставки, полагается на поведение ADNL
- **Допустим reordering** — фреймы разных потоков могут приходить в любом порядке
- **Multiplexing** — `stream_id` как ключ демультиплексирования (до 2^32 одновременных потоков)
- **Concatenation** — несколько фреймов могут быть в одном ADNL-сообщении

---

## 3. Архитектура Android-клиента

### 3.1 Стек технологий (Android)

```
Kotlin (приложение)
  ├── TonShieldVpnService.kt     — системный VPN-сервис
  ├── LocalSocks5Server.kt       — SOCKS5 на 127.0.0.1:1080
  ├── NativeTonTransport.kt      — JNI-обёртка над C++
  └── Tun2SocksNative.kt         — JNI для badvpn-tun2socks (lwIP)
        ↓ JNI
C++ (нативный слой)
  ├── ton_transport.cpp          — JNI bridge (Kotlin ↔ ADNL)
  ├── ton_adnl_client_backend.cpp — ADNL-клиент (UDP peer)
  └── tun2socks_bridge.cpp       — запуск badvpn-tun2socks
        ↓ Линковка
TON SDK (C++)
  ├── adnl                       — ADNL протокол (peer/channel)
  ├── dht                        — DHT для ADNL
  ├── tdactor                    — Actor framework (главный цикл событий)
  ├── tdutils                    — Утилиты (буферы, IP, Clocks)
  ├── tdnet                      — Сетевой слой (UDP/TCP)
  ├── ton_crypto/keys            — Ed25519 ключи, шифрование
  └── OpenSSL (libcrypto)        — Криптографические примитивы
```

### 3.2 Слои детально

**Kotlin-слой (UI + VPN):**

| Компонент | Назначение |
|-----------|------------|
| `TonShieldVpnService` | Системный VpnService. Создаёт TUN (`0.0.0.0/0`), управляет жизненным циклом VPN |
| `LocalSocks5Server` | SOCKS5 на `127.0.0.1:1080`. Принимает CONNECT, создаёт stream в ADNL |
| `NativeTonTransport` | Менеджер HOPE-фреймов. `openStream()`, `sendStreamData()`, `closeStream()` |
| `Tun2SocksNative` | JNI-обёртка для `badvpn-tun2socks` (lwIP) |

**C++ JNI-слой:**

| Файл | Назначение |
|------|------------|
| `ton_transport.cpp` | Мост Kotlin↔ADNL. `configureAdnlPeer()`, `sendPacket()`, `receivePacket()` |
| `ton_adnl_client_backend.cpp` | Полноценный ADNL-клиент: `Scheduler`, `AdnlNetworkManager`, `Adnl`, `Keyring` |

**ADNL-клиент (`ton_adnl_client_backend.cpp`):**

Экспортирует C-API:
```c
void* ton_adnl_client_create(privkey, egress_addr, advertise_host)
int   ton_adnl_client_start(void* handle)
void  ton_adnl_client_stop(void* handle)
void  ton_adnl_client_destroy(void* handle)
int   ton_adnl_client_send(void* handle, data, len)
void  ton_adnl_client_set_on_message(void* handle, callback, user_data)
```

Внутренняя структура:
```cpp
class TonAdnlClientRuntime {
    Scheduler scheduler_;           // Actor event loop
    ActorOwn<Keyring> keyring_;     // Хранилище Ed25519 ключей
    ActorOwn<AdnlNetworkManager> network_manager_; // UDP слушатель :40000
    ActorOwn<Adnl> adnl_;           // ADNL движок
    PrivateKey local_private_key_;  // Локальный Ed25519 ключ
    AdnlNodeIdFull local_full_id_;  // Локальный ADNL ID
    AdnlNodeIdShort remote_short_id_; // ID удалённого сервера
};
```

Алгоритм инициализации:
1. Парсит `egress_adnl_address` = `pubkey_hex@host:port`
2. Создаёт `Keyring`, импортирует `PrivateKey` (Ed25519)
3. Создаёт `AdnlNetworkManager` (UDP порт 40000)
4. `Adnl::add_id(local_full_id, bind_ip:40000, category=0)`
5. `Adnl::add_peer(local_id, remote_full_id, udp_addr)`
6. `Adnl::subscribe(local_id, callback)` — приём входящих ADNL-сообщений
7. Сообщения -> `receive_message` -> `on_inbound_message` -> JNI callback -> Kotlin

### 3.3 Windows Desktop клиент (TON-Tunnel.exe)

**Стек:**
```
TON-Tunnel.exe (C++20, Win32 GUI)
  ├── WintunLwipBridge.cpp     — wintun + lwIP (TCP/IP стек)
  ├── LocalSocks5Server.cpp    — SOCKS5 на 127.0.0.1:1080
  ├── TonClientCore.cpp        — Мультиплексирование HOPE-фреймов
  └── TonProtocol.cpp          — Кодек HOPE (encode/decode)
        ↓ Прямая линковка
  ├── ton_adnl_client_backend  — ADNL клиент (DLL)
  ├── adnl / dht / tdactor     — TON SDK
  ├── lwip_core                — lwIP из badvpn (статическая библиотека)
  └── wintun.dll               — TUN-адаптер Windows
```

**Отличия от Android:**
- Вместо VpnService → **wintun** (TUN драйвер для Windows)
- Вместо badvpn-tun2socks → **собственный lwIP bridge** (wintun ↔ lwIP ↔ SOCKS5)
- SOCKS5 и HOPE-кодек на **C++** (не Kotlin)
- ADNL клиент — **прямая линковка** (без JNI, без LoadLibrary)
- GUI: **Win32 API** (кнопки START/STOP, лог-бокс)

---

## 4. Архитектура Сервера (VPS Linux)

### 4.1 Стек

```
egress_node (C++20, Linux)
  ├── egress_node.cpp           — Прикладной слой (stream dispatch)
  ├── protocol.cpp              — HOPE кодек (decode/encode)
  └── ton_adnl_backend.cpp      — ADNL backend
        ↓ Линковка
  ├── adnl / dht / tdactor      — TON SDK
  └── OpenSSL (libcrypto)
```

### 4.2 Серверный ADNL backend (`ton_adnl_backend.cpp`)

- Слушает UDP порт (например `30303`)
- Ведёт таблицу известных клиентов: `client_id` → `AdnlNodeIdShort`
- Принимает HOPE-фреймы через `receive_message` callback
- Отвечает клиенту через `Adnl::send_message(client_id, payload)`
- Поддерживает и raw-message, и query/answer режимы

### 4.3 Egress Node (`egress_node.cpp`)

**Управление потоками:**
```cpp
struct Stream {
    uint32_t stream_id;
    int tcp_socket_fd;         // fd от connect()
    std::vector<uint8_t> rx_buf; // буфер на чтение из сокета
    std::chrono::steady_clock::time_point created_at;
    bool open_acked;            // отправлен ли OPEN ACK
};
```

**Обработка OPEN:** проверка токена → проверка allowed_ports/block_private_ips → DNS → TCP connect → OPEN ACK  
**Обработка DATA:** write() в сокет → read() из сокета → encode DATA → send_message()  
**Обработка CLOSE:** закрытие сокета, удаление stream

**Политики безопасности:**
- `allowed_clients[]` — whitelist публичных ключей
- `auth_token` — общий токен авторизации
- `allowed_ports[]` — разрешённые порты (80, 443, 22...)
- `block_private_ips` — запрет 10.x, 172.16-31.x, 192.168.x
- `max_streams` — лимит одновременных потоков
- `idle_timeout_sec` — таймаут бездействия

---

## 5. Ключевые технические решения

### 5.1 Почему ADNL, а не тоннель?

| Подход | Проблема |
|--------|----------|
| Прямой TCP/UDP туннель | Пассивный DPI видит VPN-протокол |
| shadowsocks | Блокируется по паттернам трафика |
| WireGuard/OpenVPN | Сигнатуры протокола известны DPI |
| **ADNL** | Трафик выглядит как P2P-обмен сети TON |

ADNL — это **протокол транспортного уровня сети TON**, используемый всеми узлами TON для общения между собой. Трафик через ADNL:
- Зашифрован (Ed25519 + AES-CTR/ChaCha20)
- Неотличим от обычного TON P2P трафика
- Не имеет TLS/HTTP сигнатур
- Использует UDP (сложнее блокировать чем TCP)

### 5.2 Почему свой протокол (HOPE), а не стандартный?

Стандартные протоколы поверх ADNL не подходят:

| Протокол | Почему нет |
|----------|------------|
| ADNL query/answer | Синхронный, таймауты, не для потоков |
| tonlib | Слишком тяжёлый, не совместим с Android |
| AdnlExtClient | TCP-клиент, не подходит для UDP egress |
| RLDP | Reliable Large Datagram — избыточен для нашего случая |

**HOPE** — минимальный бинарный протокол (5 типов фреймов), оптимизированный для потокового TCP-over-ADNL.

### 5.3 Почему lwIP (badvpn)?

| Альтернатива | Проблема |
|--------------|----------|
| Самодельный TCP-эмулятор | Крайне сложно (SYN/ACK sequence, retransmission, window scaling...) |
| Windows TAP + tun2socks | Требует установки драйвера TAP |
| **lwIP embedded** | Полноценный TCP/IP стек, без внешних зависимостей |

lwIP — это TCP/IP стек для встраиваемых систем, используемый в badvpn-tun2socks. Он реализует полноценный TCP (SYN/SYN-ACK/ACK/FIN/RST, retransmission, congestion control), что позволяет корректно обрабатывать TUN-пакеты без ручной эмуляции.

### 5.4 NAT/UDP проблема (Android)

Android за NAT не может рекламировать публичный адрес в ADNL — внешний UDP source port перезаписывается NAT'ом, отличаясь от локального порта. Решение:
- `local_advertise_host` **отключён** для публичного интернета
- Реклама работает только в LAN-сценарии (тот же /24)
- В остальных случаях: `no-advertise` — сервер узнаёт адрес клиента из входящего UDP-пакета

---

## 6. Структура проекта на диске

```
DapfTONGate/
├── app/                              # Android-клиент
│   ├── src/main/java/.../tongate/
│   │   ├── data/
│   │   │   ├── native/NativeTonTransport.kt    # JNI-обёртка HOPE
│   │   │   └── vpn/
│   │   │       ├── TonShieldVpnService.kt      # VpnService
│   │   │       ├── LocalSocks5Server.kt        # SOCKS5
│   │   │       ├── Tun2SocksNative.kt          # JNI tun2socks
│   │   │       └── ...
│   │   └── ui/
│   │       ├── MainActivity.kt                 # UI (старт/стоп)
│   │       └── VpnViewModel.kt                 # Состояние VPN
│   └── src/main/cpp/                           # Нативный C++ слой
│       ├── ton_adnl_client_backend.cpp         # ADNL клиент (UDP peer)
│       ├── ton_transport.cpp                   # JNI bridge
│       └── tun2socks_bridge.cpp                # JNI для badvpn
│
├── TON-Tunnel/                      # Windows Desktop клиент
│   ├── TON-Tunnel.cpp               # Win32 GUI + оркестрация
│   ├── LocalSocks5Server.cpp        # SOCKS5 сервер (C++)
│   ├── TonClientCore.cpp            # Мультиплексор HOPE-фреймов
│   ├── TonProtocol.cpp              # HOPE кодек (encode/decode)
│   ├── WintunLwipBridge.cpp         # wintun + lwIP bridge (новый)
│   ├── lwipopts.h                   # Конфиг lwIP для Windows
│   └── third_party/
│       ├── badvpn/                  # lwIP + tun2socks
│       └── wintun.h                 # Windows TUN driver
│
├── ton-egress-node/                 # Сервер (Linux VPS)
│   ├── src/
│   │   ├── egress_node.cpp          # Прикладной слой (stream dispatch)
│   │   ├── ton_adnl_backend.cpp     # ADNL backend
│   │   └── protocol.cpp             # HOPE кодек
│   ├── include/protocol.hpp
│   ├── config.json                  # Конфигурация
│   └── third_party/ton/             # TON SDK
│
├── PROTOCOL.md                      # Спецификация HOPE
├── PROJECT_OVERVIEW_RU.md           # Этот документ
└── BUILD.md                         # Инструкции по сборке
```

---

## 7. Магия: как ADNL реально доставляет трафик

### 7.1 Что происходит при нажатии START

**Android:**
```
1. TonShieldVpnService.startVpn()
2.   VpnService.establish()          → создаёт TUN (fd)
3.   tun2socks(tunFd, socks5:1080)   → lwIP TCP/IP стек
4.   LocalSocks5Server.start(1080)
5.   NativeTonTransport.initAdnl()
6.     → ton_adnl_client_create()    → Scheduler, Adnl, Keyring
7.     → ton_adnl_client_start()     → UDP слушатель :40000
8.     → subscribe()                 → приём ADNL сообщений
9.   Маршрут 0.0.0.0/0 → TUN
```

**Windows:**
```
1. startTunnel()
2.   ton_adnl_client_create()        → ADNL клиент (прямая линковка)
3.   TonClientCore.start()           → приёмник фреймов
4.   LocalSocks5Server.start(1080)   → SOCKS5 сервер
5.   WintunLwipBridge.start()
6.     → wintun.dll                  → TUN адаптер "TON-Tunnel"
7.     → lwip_init()                 → TCP/IP стек
8.     → netif 10.0.0.1/24           → lwIP интерфейс
9.     → readThread                  → чтение из TUN → lwIP
10.  Системный прокси → SOCKS5 127.0.0.1:1080
11.  route add 0.0.0.0/0 → 10.0.0.1 (TUN)
```

### 7.2 Что происходит при открытии google.com

```
ТЕЛЕФОН: App → connect("google.com", 443)
    ↓ Системный resolver
    ↓ Маршрут 0.0.0.0/0 → TUN
    ↓
TUN: IP пакет с TCP SYN к google.com:443
    ↓ lwIP
tun2socks: TCP handshake (SYN → SYN-ACK → ACK) с виртуальным IP
    ↓ SOCKS5 CONNECT google.com:443
LocalSocks5Server: stream_id = nextId++
    ↓ encodeOpenFrame(stream_id, "google.com", 443, auth_token)
    ↓ ton_adnl_client_send(handle, frame_bytes)
    ↓
ADNL: Adnl::send_message(local_id, remote_short_id, payload)
    ↓ UDP пакет на VPS:30303
    ↓
VPS: Adnl::receive_message() → onAdnlMessage(bytes)
    ↓ decodeInboundFrames(bytes)
    ↓ type=OPEN, stream_id=42
EgressNode: открыть TCP connect("google.com", 443)
    ↓ TCP connected
    ↓ encodeOpenAckFrame(42) → send_message
    ↓
ADNL: UDP пакет обратно на телефон
    ↓ onAdnlMessage → queue
NativeTonTransport: OPEN ACK (stream=42) → поток открыт!
    ↓
TUN: SYN-ACK ← lwIP
    ↓ ACK → google.com

Данные:
Phone → DATA(42, HTTP-request) → ADNL → VPS → write(sock)
VPS → read(sock) → DATA(42, HTTP-response) → ADNL → Phone → TUN
```

---

## 8. Производительность и текущие ограничения

### 8.1 Что работает

✅ ADNL связь Android ↔ VPS установлена и стабильна  
✅ HOPE OPEN → TCP connect на VPS работает  
✅ DATA релей в обе стороны  
✅ CLOSE корректное закрытие потоков  
✅ Обработка ошибок (ERROR фреймы)  
✅ Мультиплексирование (одновременные потоки)  
✅ Windows Desktop клиент (в процессе доводки)

### 8.2 Что медленно (и почему)

| Проблема | Причина | Решение |
|----------|---------|---------|
| Задержка OPEN | Ожидание 3-way handshake (ADNL + TCP) | Pre-connect pool |
| JNI overhead | Каждый пакет через JNI | Direct ByteBuffer |
| Малые буферы | chunk_size=1200 байт | Увеличить до MTU |
| Нет batching | По одному фрейму за send_message | Конкатенация фреймов |
| Нет zero-copy | Копирование payload на каждом уровне | Scatter/gather buffers |

### 8.3 Roadmap

- [x] Android ↔ VPS ADNL связь
- [x] HOPE протокол (OPEN/DATA/CLOSE/ERROR)
- [x] SOCKS5 сервер
- [x] TUN + lwIP интеграция
- [ ] Оптимизация производительности (batching, zero-copy)
- [ ] Windows Desktop клиент (в процессе)
- [ ] Поддержка UDP (DNS через ADNL)
- [ ] Поддержка нескольких egress-нод (failover)
- [ ] Обфускация ADNL трафика

---

## 9. Почему это круто (для знатоков)

1. **Не очередной прокси** — здесь кастомный стек от Ed25519 до TCP connect
2. **Настоящий ADNL** — не имитация: `Adnl::add_peer()`, `AdnlNetworkManager`, `subscribe()`, реальные `send_message` через UDP
3. **Свой протокол поверх ADNL** — HOPE (5 типов фреймов, multiplexing, stream state machine)
4. **Две платформы**: Android (Kotlin+JNI+C++) и Windows (Win32+C++20)
5. **Встроенный TCP/IP стек** (lwIP из badvpn) без внешних зависимостей
6. **Обход NAT** для мобильного интернета (no-advertise, server-side address discovery)
7. **Полная кодовая база** — от UI до ADNL, без внешних бинарных блобов (кроме OpenSSL)

---

## 10. Как собрать

### Android
```bash
cd app/
./gradlew assembleDebug
# или
./gradlew assembleRelease
```

### Windows (TON-Tunnel.exe)
```cmd
cd TON-Tunnel
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Strawberry\perl\bin;%PATH%
cmake -B build-backend-msvc -G "Visual Studio 17 2022" -A x64 ^
    -DPERL_EXECUTABLE=C:/Strawberry/perl/bin/perl.exe
cmake --build build-backend-msvc --config Release
```

### Сервер (Linux VPS)
```bash
cd ton-egress-node/
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## 11. Конфигурация

### Android (`adnl_transport_config.json`)
```json
{
  "egress_adnl_address": "pubkey_hex@vps_ip:30303",
  "client_private_key": "ed25519_private_key_hex",
  "auth_token": "shared_secret_token",
  "local_udp_advertise_host": ""
}
```

### Сервер (`config.json`)
```json
{
  "private_key": "server_ed25519_key_hex",
  "listen_port": 30303,
  "max_streams": 256,
  "idle_timeout_sec": 300,
  "auth_token": "shared_secret_token",
  "egress": {
    "allowed_ports": [80, 443, 22, 8080],
    "block_private_ips": true
  }
}
```

---

## 12. Заключение

TON Tunnel — это **работающий proof-of-concept** транспорта пользовательского TCP-трафика через сеть TON/ADNL. Проект прошёл путь от "ничего не работает" до "трафик реально идёт через ADNL", преодолев десятки технических препятствий: сборка TON SDK под Android, NAT/UDP проблемы, генерация TL-артефактов, интеграция lwIP.

Это не просто "прокси с модным названием" — это полноценный VPN-туннель, использующий криптографию и транспорт реальной сети TON. Протокол HOPE, реализованный поверх ADNL, минимален, эффективен и расширяем.

**Автор:** lucky  
**Статус:** активно дорабатывается (Windows-клиент, оптимизация)  
