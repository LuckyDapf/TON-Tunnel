# TON Egress Node (VPS) — установка на русском

Этот сервер нужен как выходная точка VPN-трафика: Android-клиент шлет фреймы через TON/ADNL, а VPS открывает TCP-соединения в интернет.

## Что это за сервис (кратко)

- принимает `OPEN/DATA/CLOSE/ERROR` от клиента
- на `OPEN` делает DNS на VPS и открывает TCP
- на `DATA` передает байты в сокет
- ответ из сокета отправляет обратно клиенту
- это **не** публичный SOCKS/HTTP proxy

## Установка на VPS (Debian/Ubuntu) — пошагово

### 1) Установить системные пакеты

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config libssl-dev zlib1g-dev
```

### 2) Склонировать проект и подтянуть submodules

```bash
git submodule update --init --recursive
```

### 3) Собрать сервер

Если собираете обычный вариант:

```bash
mkdir -p build
cd build
cmake ..
make -j
```

Если нужен ADNL backend (`libton_adnl_backend.so`):

```bash
mkdir -p build
cd build
cmake .. -DTON_EGRESS_BUILD_ADNL_BACKEND=ON
make -j
```

После сборки должен появиться бинарник `ton_egress_node`.

## Быстрый запуск без systemd (проверка)

```bash
./ton_egress_node --config ../config.json
```

Если старт успешный — переходите к установке как сервис.

## Установка как systemd-сервис (рекомендуется)

### 1) Подготовить папку и скопировать файлы

```bash
sudo mkdir -p /opt/ton-egress
sudo cp ton_egress_node /opt/ton-egress/
sudo cp config.json /opt/ton-egress/
```

### 2) Создать unit-файл

Создайте файл `/etc/systemd/system/ton-egress.service`:

```ini
[Unit]
Description=TON Egress Node
After=network.target

[Service]
WorkingDirectory=/opt/ton-egress
ExecStart=/opt/ton-egress/ton_egress_node --config /opt/ton-egress/config.json
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

### 3) Включить и запустить сервис

```bash
sudo systemctl daemon-reload
sudo systemctl enable ton-egress
sudo systemctl start ton-egress
```

### 4) Проверить статус и логи

```bash
sudo systemctl status ton-egress
journalctl -u ton-egress -f
```

## Подготовка ключей и токена

Сгенерировать базовые значения:

```bash
sudo mkdir -p /opt/ton-egress
openssl rand -base64 32 > /opt/ton-egress/private.key
openssl rand -hex 32 > /opt/ton-egress/auth.token
```

В `/opt/ton-egress/config.json` подставить:

- `private_key` = содержимое `/opt/ton-egress/private.key`
- `auth_token` = содержимое `/opt/ton-egress/auth.token`
- `listen_port` = порт сервера (например `30303`)

## Что брать для Android-конфига

После запуска сервера:

```bash
sudo systemctl restart ton-egress
journalctl -u ton-egress -n 120 --no-pager
```

Из логов взять ADNL публичный ключ (`pubkey_hex`, 64 hex-символа).

Сформировать адрес для Android в формате:

- `<pubkey_hex>@<host:port>`
- пример: `7f1c...64hex...9ab3@203.0.113.10:30303`

Где:

- `pubkey_hex` — ключ из логов сервера
- `host` — публичный IP (или домен) вашего VPS
- `port` — `listen_port` из `config.json`

Заполнить Android файл:

- путь: `app/src/main/res/raw/adnl_transport_config.json`
- поля:
  - `egress_adnl_address`: `<pubkey_hex>@<host:port>`
  - `client_private_key`: ключ Android-клиента
  - `auth_token`: тот же, что на VPS

## Полезные команды

- Перезапуск сервиса: `sudo systemctl restart ton-egress`
- Остановка: `sudo systemctl stop ton-egress`
- Логи: `journalctl -u ton-egress -f`
- Последние 120 строк: `journalctl -u ton-egress -n 120 --no-pager`

## Безопасность (уже заложено)

- блок приватных подсетей:
  - `127.0.0.1`
  - `10.0.0.0/8`
  - `172.16.0.0/12`
  - `192.168.0.0/16`
  - `169.254.169.254`
- ограничения по потокам и idle timeout
- auth по `allowed_clients` и/или `auth_token`

## Лицензирование монорепозитория

Сервер является частью проекта **DapfTON / TON Tunnel**. Корневые условия лицензии и список сторонних библиотек:

- AGPL-3.0 (англ.), [`../LICENSE`](../LICENSE)
- Сводка third-party — [`../THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md)

**Не используйте** ручное `touch` для `.openssl_quic_checked` и не патчите пути сборки без причины: при ошибках OpenSSL/sodium выполните чистый `rm -rf build` и заново запустите `cmake`/`make`.
