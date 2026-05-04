# Блок About на GitHub — что куда вставить (русские подсказки)

Интерфейс GitHub на английском, текст ниже можно **копировать** в поля.

## Короткое описание репозитория (поле Description)

Подойдёт такой текст (до ~350 символов):

```
Мультиплексный туннель поверх ADNL: Android VPN + локальный SOCKS5, клиент для Windows (`TON-Tunnel/`, C++, Wintun),
протокол OPEN/DATA/CLOSE/ERROR; Linux egress (C++) и опционально libton_adnl_backend. AGPL-3.0.
```

**Ещё короче:**

```
Туннель TON ADNL: Android + Windows C++ клиент — узел egress на Linux. AGPL-3.0.
```

## Topics (тематические метки)

Вставляйте по одной через Enter или просто добавьте вручную. Рекомендуемые:

```
ton blockchain adnl android windows vpn socks5 cpp kotlin ndk cmake wintun win32 linux-server AGPL
```

Минимальный набор:

```
ton adnl vpn android socks5 tunnel cpp AGPL
```

## Website

Полезно добавить только если есть **официальный** сайт, документация или блог. Иначе оставьте пустым.

## Русскоязычный README для пользователей

Основной обзор: [`README.ru.md`](../README.ru.md)  
Сервер на VPS: [`../ton-egress-node/README.md`](../ton-egress-node/README.md)

Английский аналог шпаргалки About: **[GITHUB_ABOUT.md](GITHUB_ABOUT.md)**
