#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="$PROJECT_DIR/.build-tools"
JDK_DIR="$TOOLS_DIR/windows-jdk-21"
JDK_ZIP="$TOOLS_DIR/windows-jdk-21.zip"
CLASSES_DIR="$PROJECT_DIR/build/classes"
JAR_FILE="$PROJECT_DIR/build/big-head-vpn.jar"

command -v javac >/dev/null || { echo "Нужен JDK 21 внутри WSL (javac не найден)."; exit 1; }
command -v curl >/dev/null || { echo "Не найден curl."; exit 1; }
command -v unzip >/dev/null || { echo "Не найден unzip."; exit 1; }

mkdir -p "$TOOLS_DIR" "$PROJECT_DIR/build" "$PROJECT_DIR/dist"

if [[ ! -f "$JDK_DIR/bin/jpackage.exe" ]]; then
  echo "Скачиваю Windows JDK 21..."
  curl -fL "https://api.adoptium.net/v3/binary/latest/21/ga/windows/x64/jdk/hotspot/normal/eclipse" -o "$JDK_ZIP"
  rm -rf "$JDK_DIR" "$TOOLS_DIR/jdk-unpack"
  mkdir -p "$JDK_DIR" "$TOOLS_DIR/jdk-unpack"
  unzip -q "$JDK_ZIP" -d "$TOOLS_DIR/jdk-unpack"
  JPACKAGE_FOUND="$(find "$TOOLS_DIR/jdk-unpack" -type f -iname jpackage.exe -print -quit)"
  [[ -n "$JPACKAGE_FOUND" ]] || { echo "jpackage.exe отсутствует в архиве JDK."; exit 1; }
  cp -a "$(dirname "$(dirname "$JPACKAGE_FOUND")")/." "$JDK_DIR/"
fi

echo "Компилирую Java..."
rm -rf "$CLASSES_DIR"
mkdir -p "$CLASSES_DIR"
mapfile -t JAVA_SOURCES < <(find "$PROJECT_DIR/src/main/java" -type f -name '*.java' -print)
javac --release 21 -encoding UTF-8 -d "$CLASSES_DIR" "${JAVA_SOURCES[@]}"
if [[ -d "$PROJECT_DIR/src/main/resources" ]]; then
  cp -a "$PROJECT_DIR/src/main/resources/." "$CLASSES_DIR/"
fi
jar --create --file "$JAR_FILE" --main-class app.bighead.vpn.Main -C "$CLASSES_DIR" .

# Для изменений Java-кода не требуется каждый раз пересоздавать 200-МБ runtime.
# Обновляем JAR внутри уже собранной portable-папки и завершаемся за секунды.
if [[ "${1:-}" != "--full" \
  && -f "$PROJECT_DIR/dist/BigHeadVPN/BigHeadVPN.exe" \
  && -d "$PROJECT_DIR/dist/BigHeadVPN/runtime" \
  && -f "$PROJECT_DIR/dist/BigHeadVPN/app/big-head-vpn.jar" ]]; then
  cp "$JAR_FILE" "$PROJECT_DIR/dist/BigHeadVPN/app/big-head-vpn.jar"
  cp "$PROJECT_DIR/THIRD_PARTY_NOTICES.md" "$PROJECT_DIR/dist/BigHeadVPN/"
  echo
  echo "Быстрая сборка готова: $PROJECT_DIR/dist/BigHeadVPN/BigHeadVPN.exe"
  echo "Для полного пересоздания EXE: ./scripts/build-windows-wsl.sh --full"
  exit 0
fi

WINDOWS_USER="${WINDOWS_USER:-}"
if [[ -z "$WINDOWS_USER" ]]; then
  WINDOWS_USER="$(cd /mnt/c && powershell.exe -NoProfile -Command '[Environment]::UserName' 2>/dev/null | tr -d '\r' | tail -n 1)"
fi
if [[ -z "$WINDOWS_USER" || ! -d "/mnt/c/Users/$WINDOWS_USER" ]]; then
  echo "Не удалось определить Windows-пользователя."
  echo "Запустите так: WINDOWS_USER=ваше_имя ./scripts/build-windows-wsl.sh"
  exit 1
fi
echo "Windows-пользователь: $WINDOWS_USER"
WINDOWS_STAGE="/mnt/c/Users/$WINDOWS_USER/AppData/Local/Temp/big-head-vpn-wsl-build"
rm -rf "$WINDOWS_STAGE"
mkdir -p "$WINDOWS_STAGE/input" "$WINDOWS_STAGE/output"
cp "$JAR_FILE" "$WINDOWS_STAGE/input/big-head-vpn.jar"
cp "$PROJECT_DIR/packaging/big-head-vpn.ico" "$WINDOWS_STAGE/input/big-head-vpn.ico"

echo "Создаю Windows EXE со встроенной Java..."
"$JDK_DIR/bin/jpackage.exe" \
  --type app-image \
  --name BigHeadVPN \
  --input "C:\\Users\\$WINDOWS_USER\\AppData\\Local\\Temp\\big-head-vpn-wsl-build\\input" \
  --main-jar big-head-vpn.jar \
  --main-class app.bighead.vpn.Main \
  --icon "C:\\Users\\$WINDOWS_USER\\AppData\\Local\\Temp\\big-head-vpn-wsl-build\\input\\big-head-vpn.ico" \
  --dest "C:\\Users\\$WINDOWS_USER\\AppData\\Local\\Temp\\big-head-vpn-wsl-build\\output" \
  --app-version 1.0.0 \
  --vendor "Big Head"

mkdir -p "$WINDOWS_STAGE/output/BigHeadVPN/scripts"
cp "$PROJECT_DIR/scripts/download-sing-box.ps1" "$WINDOWS_STAGE/output/BigHeadVPN/scripts/"
if [[ -f "$PROJECT_DIR/dist/BigHeadVPN/tools/sing-box.exe" ]]; then
  mkdir -p "$WINDOWS_STAGE/output/BigHeadVPN/tools"
  cp -a "$PROJECT_DIR/dist/BigHeadVPN/tools/." "$WINDOWS_STAGE/output/BigHeadVPN/tools/"
else
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
    "C:\\Users\\$WINDOWS_USER\\AppData\\Local\\Temp\\big-head-vpn-wsl-build\\output\\BigHeadVPN\\scripts\\download-sing-box.ps1"
fi
cp "$PROJECT_DIR/THIRD_PARTY_NOTICES.md" "$WINDOWS_STAGE/output/BigHeadVPN/"

rm -rf "$PROJECT_DIR/dist/BigHeadVPN"
cp -a "$WINDOWS_STAGE/output/BigHeadVPN" "$PROJECT_DIR/dist/BigHeadVPN"

echo
echo "Готово: $PROJECT_DIR/dist/BigHeadVPN/BigHeadVPN.exe"
