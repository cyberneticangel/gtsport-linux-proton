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
exec ./GTSport "$@"
