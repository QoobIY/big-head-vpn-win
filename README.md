# Big Head VPN for Windows

## Нативная версия (C++20)

Специализированная Windows x64-версия находится в `native/`. Она не
использует Java и sing-box. Сейчас готовы нативный интерфейс, подписки/группы,
WinHTTP-загрузка, защищённое DPAPI-хранилище и компактное MsQuic-ядро.
Интерфейс использует встроенный Manrope из Google Fonts: шрифт работает
офлайн и не требует установки в Windows.
Переключатель автозагрузки в шапке создаёт задание входа в Планировщике
Windows с повышенными правами, поэтому VPN запускается без повторного UAC.
QUIC/TLS, HTTP/3/QPACK, авторизация Hysteria2 и TCP CONNECT через локальный
SOCKS5 relay проверены на официальном сервере и реальной подписке. Также готов
VLESS TCP/raw через REALITY с `xtls-rprx-vision`: реализация проверяется
дифференциальным end-to-end тестом против официального Xray. VLESS
gRPC/REALITY также проходит локальный oracle-тест и проверку реального профиля
через Discord API. Для gRPC проверяются TCP, прямой UDP-транспорт и полный
SOCKS5 UDP ASSOCIATE с двумя разными адресатами через локальный echo, а также
публичный STUN. TCP и UDP используют один общий REALITY/HTTP2 transport с
отдельными gRPC stream; oracle дополнительно проверяет восемь одновременных
TCP-stream.
Также готов
экспериментальный фильтр процессов через WinDivert: TCP и UDP поддерживают
IPv4/IPv6, а UDP передаётся через нативные Hysteria2 QUIC DATAGRAM. PID
сопоставляется с сетевым flow без проксирования трафика самого клиента.
Системный режим ещё разрабатывается.

Сборка из WSL:

```bash
./scripts/build-native-windows-wsl.sh
```

Результат:

```text
dist/BigHeadVPN-Native/BigHeadVPN.exe
dist/BigHeadVPN-Native/msquic.dll
```

LLVM-MinGW используется только при сборке и в приложение не попадает. MsQuic —
официальная QUIC-библиотека Microsoft; её лицензия кладётся рядом с EXE.
Лицензия встроенного шрифта находится рядом в `MANROPE_LICENSE.txt`.
Для фильтра процессов рядом также находятся официальный `WinDivert.dll` и
`WinDivert64.sys`. Запускайте весь каталог с локального диска Windows, а не
напрямую через `\\wsl.localhost`; Windows запросит права администратора.

Воспроизводимая проверка VLESS Vision против официального Xray:

```bash
cmake --build build/native-windows --target BigHeadVPNProbe -j2
./scripts/test-vless-vision-oracle-wsl.sh /path/to/official-xray
./scripts/test-vless-grpc-oracle-wsl.sh /path/to/official-xray
./scripts/test-vless-grpc-profile-wsl.sh \
    /path/to/official-xray subscription.txt grpc-profile-number
```

Oracle-сценарии сначала получают HTTP-ответ через официальный клиент Xray,
затем через `BigHeadVPNProbe` и требуют совпадения кодов. Последний сценарий
повторяет сравнение на выбранном реальном профиле через Discord Gateway API.
Xray используется только для тестов и в `dist` не копируется.

В разделе «Процессы через VPN» выберите запущенное приложение слева и нажмите
«Добавить». Справа остаётся сохранённый список приложений, направляемых через
VPN; двойной щелчок также добавляет или убирает строку. Подключите VPN, затем
полностью перезапустите выбранные приложения: WinDivert не получает события
сокетов, открытых до запуска фильтра.

## Логотип

Исходник: `assets/big-head-windows.svg`. Иконка `packaging/big-head-vpn.ico` используется в EXE, окне, шапке интерфейса и системном трее.

После изменения SVG выполните `python3 scripts/generate-windows-icon.py` (Linux, библиотеки librsvg и libcairo), затем `bash scripts/build-native-windows-wsl.sh`. ICO содержит размеры от 16 до 256 пикселей.

### Проверка REALITY X25519MLKEM768

`cmake --build build/native-windows --target BigHeadVPNHybridTest` и
`build/native-windows/BigHeadVPNHybridTest.exe` проверяют гибридный секрет,
границы буферов, некорректную точку X25519 и повреждённый ML-KEM ciphertext.
`scripts/test-vless-vision-oracle-wsl.sh /path/to/xray` сравнивает VLESS Vision
с официальным Xray на локальном TLS-сервере (нужен `openssl`), проверяет выбор
X25519MLKEM768 и отказ при неверном short ID. Xray используется только в тесте.

Формат гибридного обмена: https://www.ietf.org/archive/id/draft-ietf-tls-ecdhe-mlkem-04.html
Закреплённая реализация ML-KEM: `native/vendor/pqclean/UPSTREAM.md`.
