#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v powershell.exe >/dev/null || { echo "Не найден Windows PowerShell (powershell.exe)."; exit 1; }
bash "$PROJECT_DIR/scripts/build-native-windows-wsl.sh"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$PROJECT_DIR/scripts/build-windows-installer.ps1")" "$@"
