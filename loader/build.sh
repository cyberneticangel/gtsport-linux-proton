#!/usr/bin/env bash
# Builds RyGT.exe (the loader) on Linux with Mono's C# compiler (Arch/CachyOS: pacman -S mono), the
# same sources and references as build.cmd. The result runs on Windows' .NET Framework and under
# Proton/Wine (wine-mono).
# Usage: build.sh [output folder]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here}"
csc="$(command -v csc || command -v mcs || true)"
if [[ -z "$csc" ]]; then
    echo "build.sh: no C# compiler; install Mono (csc or mcs)" >&2
    exit 1
fi
"$csc" -nologo -target:winexe -optimize+ -win32icon:"$here/ryGT.ico" -resource:"$here/ryGT.ico",ryGT.ico -out:"$out/RyGT.exe" \
    -r:System.dll -r:System.Core.dll -r:System.Drawing.dll -r:System.Windows.Forms.dll -r:System.Web.Extensions.dll \
    "$here/RyGTLoader.cs" "$here/ControlsForm.cs" "$here/PortsForm.cs"
echo "$out/RyGT.exe"
