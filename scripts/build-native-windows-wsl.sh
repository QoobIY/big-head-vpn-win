#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="$PROJECT_DIR/.build-tools"
TOOLCHAIN_DIR="$TOOLS_DIR/llvm-mingw"
ARCHIVE="$TOOLS_DIR/llvm-mingw.tar.xz"
BUILD_DIR="$PROJECT_DIR/build/native-windows"
DIST_DIR="$PROJECT_DIR/dist/BigHeadVPN-Native"
TOOLCHAIN_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64.tar.xz"
TOOLCHAIN_SHA256="534b92e067b22a6b4441f48ae9240a3341b17825d04d577eab0cf85c44b4deda"
MSQUIC_VERSION="2.5.9"
MSQUIC_PACKAGE="$TOOLS_DIR/msquic.$MSQUIC_VERSION.nupkg"
MSQUIC_DIR="$TOOLS_DIR/msquic"
MSQUIC_URL="https://www.nuget.org/api/v2/package/Microsoft.Native.Quic.MsQuic.Schannel/$MSQUIC_VERSION"
MSQUIC_SHA256="9877a919e1aa73aa4800f1e8a06b6539021f376228910138eb04540cd956617b"
LSQPACK_DIR="$TOOLS_DIR/ls-qpack"
LSQPACK_ARCHIVE="$TOOLS_DIR/ls-qpack-771a794.tar.gz"
LSQPACK_URL="https://codeload.github.com/litespeedtech/ls-qpack/tar.gz/771a794d7dfcc6132576136f62db3065493d876f"
LSQPACK_SHA256="fd91d0150a14ec4e5df13ad78843b107d8583cf0ea0981f48b3ac4bc5b59bd75"
NGHTTP2_VERSION="1.69.0"
NGHTTP2_DIR="$TOOLS_DIR/nghttp2"
NGHTTP2_ARCHIVE="$TOOLS_DIR/nghttp2-$NGHTTP2_VERSION.tar.xz"
NGHTTP2_URL="https://github.com/nghttp2/nghttp2/releases/download/v$NGHTTP2_VERSION/nghttp2-$NGHTTP2_VERSION.tar.xz"
NGHTTP2_SHA256="1fb324b6ec2c56f6bde0658f4139ffd8209fa9e77ce98fd7a5f63af8d0e508ad"
WINDIVERT_DIR="$TOOLS_DIR/windivert"
WINDIVERT_ARCHIVE="$TOOLS_DIR/WinDivert-2.2.2-A.zip"
WINDIVERT_URL="https://github.com/basil00/WinDivert/releases/download/v2.2.2/WinDivert-2.2.2-A.zip"
WINDIVERT_SHA256="63cb41763bb4b20f600b6de04e991a9c2be73279e317d4d82f237b150c5f3f15"

command -v cmake >/dev/null || { echo "Не найден cmake."; exit 1; }
mkdir -p "$TOOLS_DIR"

if [[ ! -x "$TOOLCHAIN_DIR/bin/x86_64-w64-mingw32-clang++" ]]; then
  command -v curl >/dev/null || { echo "Не найден curl."; exit 1; }
  echo "Скачиваю portable LLVM-MinGW (только инструмент сборки)..."
  curl -fL "$TOOLCHAIN_URL" -o "$ARCHIVE"
  echo "$TOOLCHAIN_SHA256  $ARCHIVE" | sha256sum -c -
  rm -rf "$TOOLCHAIN_DIR"
  mkdir -p "$TOOLCHAIN_DIR"
  tar -xJf "$ARCHIVE" --strip-components=1 -C "$TOOLCHAIN_DIR"
fi

if [[ ! -f "$LSQPACK_DIR/lsqpack.c" ]]; then
  echo "Скачиваю закреплённый QPACK decoder..."
  curl -fL "$LSQPACK_URL" -o "$LSQPACK_ARCHIVE"
  echo "$LSQPACK_SHA256  $LSQPACK_ARCHIVE" | sha256sum -c -
  rm -rf "$LSQPACK_DIR"
  mkdir -p "$LSQPACK_DIR"
  tar -xzf "$LSQPACK_ARCHIVE" --strip-components=1 -C "$LSQPACK_DIR"
fi

if [[ ! -f "$NGHTTP2_DIR/lib/nghttp2_session.c" ]]; then
  echo "Скачиваю закреплённый nghttp2 $NGHTTP2_VERSION..."
  curl -fL "$NGHTTP2_URL" -o "$NGHTTP2_ARCHIVE"
  echo "$NGHTTP2_SHA256  $NGHTTP2_ARCHIVE" | sha256sum -c -
  rm -rf "$NGHTTP2_DIR"
  mkdir -p "$NGHTTP2_DIR"
  tar -xJf "$NGHTTP2_ARCHIVE" --strip-components=1 -C "$NGHTTP2_DIR"
fi

if [[ ! -f "$MSQUIC_DIR/bin/msquic.dll" || ! -f "$MSQUIC_DIR/include/msquic.h" ]]; then
  command -v unzip >/dev/null || { echo "Не найден unzip."; exit 1; }
  echo "Скачиваю Microsoft MsQuic $MSQUIC_VERSION..."
  curl -fL "$MSQUIC_URL" -o "$MSQUIC_PACKAGE"
  echo "$MSQUIC_SHA256  $MSQUIC_PACKAGE" | sha256sum -c -
  rm -rf "$MSQUIC_DIR"
  mkdir -p "$MSQUIC_DIR/include" "$MSQUIC_DIR/bin"
  unzip -jo "$MSQUIC_PACKAGE" 'build/native/include/msquic.h' 'build/native/include/msquic_winuser.h' -d "$MSQUIC_DIR/include"
  unzip -jo "$MSQUIC_PACKAGE" 'build/native/bin/x64/msquic.dll' 'LICENSE' -d "$MSQUIC_DIR/bin"
fi

if [[ ! -f "$WINDIVERT_DIR/WinDivert.dll" || ! -f "$WINDIVERT_DIR/WinDivert64.sys" || ! -f "$WINDIVERT_DIR/windivert.h" ]]; then
  echo "Скачиваю официальный WinDivert 2.2.2..."
  curl -fL "$WINDIVERT_URL" -o "$WINDIVERT_ARCHIVE"
  echo "$WINDIVERT_SHA256  $WINDIVERT_ARCHIVE" | sha256sum -c -
  rm -rf "$WINDIVERT_DIR"
  mkdir -p "$WINDIVERT_DIR"
  unzip -jo "$WINDIVERT_ARCHIVE" \
    'WinDivert-2.2.2-A/include/windivert.h' \
    'WinDivert-2.2.2-A/x64/WinDivert.dll' \
    'WinDivert-2.2.2-A/x64/WinDivert64.sys' \
    'WinDivert-2.2.2-A/LICENSE' -d "$WINDIVERT_DIR"
fi

cmake -S "$PROJECT_DIR/native" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchains/llvm-mingw-x64.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"
cp "$BUILD_DIR/BigHeadVPN.exe" "$DIST_DIR/BigHeadVPN.exe"
cp "$MSQUIC_DIR/bin/msquic.dll" "$DIST_DIR/msquic.dll"
cp "$MSQUIC_DIR/bin/LICENSE" "$DIST_DIR/MSQUIC_LICENSE.txt"
cp "$WINDIVERT_DIR/WinDivert.dll" "$DIST_DIR/WinDivert.dll"
cp "$WINDIVERT_DIR/WinDivert64.sys" "$DIST_DIR/WinDivert64.sys"
cp "$WINDIVERT_DIR/LICENSE" "$DIST_DIR/WINDIVERT_LICENSE.txt"
cp "$PROJECT_DIR/native/resources/MANROPE_OFL.txt" "$DIST_DIR/MANROPE_LICENSE.txt"
cp "$PROJECT_DIR/native/README_USER.md" "$DIST_DIR/README.md"

echo
echo "Готово: $DIST_DIR/BigHeadVPN.exe"
du -h "$DIST_DIR/BigHeadVPN.exe" "$DIST_DIR/msquic.dll"
