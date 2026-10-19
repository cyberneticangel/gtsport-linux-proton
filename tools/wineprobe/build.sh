#!/usr/bin/env bash
# Builds wineprobe.exe with a MinGW-w64 cross compiler (Arch/CachyOS: pacman -S mingw-w64-gcc).
# Usage: build.sh [output folder]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here}"
cc="${CC:-x86_64-w64-mingw32-gcc}"
"$cc" -O2 -std=c11 -Wall -Wextra -Wno-cast-function-type -static -o "$out/wineprobe.exe" "$here/wineprobe.c"
echo "$out/wineprobe.exe"
