#!/usr/bin/env bash
# rygt-native.sh: runs a native Linux build of GT Sport (no Wine).
#
#   rygt-native.sh [native RyGT folder] [args]
#
# The folder holds GTSport (converted with ryty without --windows), libs/ (the runtime's and
# shadGT's Linux libraries) and app0/. The game writes logs/stdout.log and logs/stderr.log, the
# renderer user/log/shadgt_*.log. Experimental: see docs/gt-sport/LINUX.md.
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

folder="${1:-$PWD}"
[[ $# -gt 0 ]] && shift
folder="$(cd "$folder" && pwd -P)"
[[ -x "$folder/GTSport" ]] || { echo "rygt-native: no GTSport executable in $folder" >&2; exit 1; }
cd "$folder"
# The runtime libraries find each other in libs/ (they carry no search path of their own yet).
export LD_LIBRARY_PATH="$folder/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# AMD GPUs: the direct renderer imports aliases of the game's GPU memory (a memfd), which amdgpu
# refuses without rygt-userptr.so (see rygt-userptr.c). Natively the aliases are never protected
# or remapped, so the imports stay valid. RYGT_USERPTR=0 turns it off.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${RYGT_USERPTR:-auto}" != 0 ]] && grep -qx 0x1002 /sys/class/drm/card*/device/vendor 2>/dev/null; then
    library="${XDG_DATA_HOME:-$HOME/.local/share}/rygt/rygt-userptr.so"
    if [[ ! -f "$library" || "$here/rygt-userptr.c" -nt "$library" ]]; then
        mkdir -p "${library%/*}"
        cc -O2 -shared -fPIC -o "$library" "$here/rygt-userptr.c" -ldl ||
            { echo "rygt-native: could not build $library (install gcc); continuing without it" >&2; library=""; }
    fi
    [[ -n "$library" ]] && export LD_PRELOAD="$library${LD_PRELOAD:+:$LD_PRELOAD}"
fi
exec ./GTSport "$@"
