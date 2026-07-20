# Big Head VPN for Windows (Java)

Windows-клиент на чистой Java 21/Swing в стиле Android-версии `../big-head-vpn`.

- VLESS: TCP/raw, HTTP/H2, WebSocket, gRPC, HTTPUpgrade, TLS и Reality.
- Hysteria2: ссылки `hysteria://`, `hysteria2://` и `hy2://`.
- Системный TUN через `sing-box`.
- VPN для всей системы либо только для выбранных процессов по имени `.exe`.
- Выбор уже запущенных процессов и текстовый фильтр.
- Локальный SOCKS5/HTTP (`mixed`) listener на выбранных IP и порту.
- Группы HTTPS-подписок: сохранение URL, обновление и удаление всей группы вместе с серверами.

## Запуск на Windows

Установите JDK 21, затем выполните:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1
```

Это единственная необходимая команда: скрипт запросит права администратора, при необходимости скачает `sing-box`, каждый раз пересоберёт Java-приложение и запустит его. Администратор нужен для TUN и системных маршрутов.

Для отдельной portable-папки:

```powershell
.\scripts\download-sing-box.ps1
.\scripts\package.ps1
```

Результат: `dist\Big Head VPN`. `jpackage` добавляет собственный Java runtime, поэтому на целевом компьютере JDK уже не требуется. Запускайте приложение через `scripts\run.ps1` либо ярлык с правами администратора.

## Сборка Windows EXE из WSL

```bash
chmod +x scripts/build-windows-wsl.sh
./scripts/build-windows-wsl.sh
```

При наличии `dist/BigHeadVPN` выполняется быстрая инкрементальная сборка: обновляется только JAR внутри готового приложения. Для полного пересоздания EXE, runtime и иконки:

```bash
./scripts/build-windows-wsl.sh --full
```

Полная сборка скачивает Windows JDK в `.build-tools`, запускает Windows `jpackage.exe`, добавляет `sing-box` и UAC-манифест. Результат: `dist/BigHeadVPN/BigHeadVPN.exe`.

Настройки: `%LOCALAPPDATA%\BigHeadVpn\settings.properties`. Runtime-конфиг с ключами находится в той же папке — не публикуйте его.

Listener на `0.0.0.0` доступен другим устройствам сети и не имеет аутентификации. Для доступа только с этого компьютера используйте `127.0.0.1`.
