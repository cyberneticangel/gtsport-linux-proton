#!/usr/bin/env bash
# extract-pkg.sh: extracts a (fake-signed) GT Sport PKG on Linux into a folder Set up can use.
#
#   extract-pkg.sh <game.pkg> <output folder>
#
# Writes the game's files to <output folder>/uroot and its plain sce_sys entries (param.sfo,
# trophy, icons, ...) to <output folder>/uroot/sce_sys, with the names a PS4 dump has. Pass
# <output folder>/uroot to setup as --game or --update.
#
# Needs LibOrbisPkg's PkgTool built with liborbispkg-linux.patch (next to this script):
#   git clone https://github.com/maxton/LibOrbisPkg && cd LibOrbisPkg && git checkout 6434772
#   git apply <RyGT>/tools/linux/liborbispkg-linux.patch
#   dotnet publish PkgTool.Core/PkgTool.Core.csproj -c Release -o ~/.local/share/rygt/pkgtool
# Environment: PKGTOOL (PkgTool.Core.dll, default ~/.local/share/rygt/pkgtool/PkgTool.Core.dll),
# DOTNET (default: dotnet on PATH).
#
# The patch fixes two things that otherwise break the game silently or loudly:
#   - PFSCReader read each compressed 64 KiB block with one Stream.Read; since .NET 6 that can
#     return less, and the rest of the block stayed zero (scattered holes in eboot.bin, gt.idx,
#     the volumes). It now reads the whole block or stops with an error.
#   - PKGTOOL_PFS_FLAGS_XOR=<hex> flips bits of the PKG's pfs_flags. The 1.69 update sets the
#     "new crypto" bit, and LibOrbisPkg then derives the wrong keys ("inode 0 is corrupt"); this
#     script retries with that bit cleared.
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

die() { echo "extract-pkg: $*" >&2; exit 1; }

[[ $# -eq 2 ]] || die "usage: extract-pkg.sh <game.pkg> <output folder>"
pkg="$1"
out="$2"
pkgtool="${PKGTOOL:-${XDG_DATA_HOME:-$HOME/.local/share}/rygt/pkgtool/PkgTool.Core.dll}"
dotnet="${DOTNET:-$(command -v dotnet || true)}"
[[ -f "$pkg" ]] || die "no such file: $pkg"
[[ -f "$pkgtool" ]] || die "PkgTool not found at $pkgtool (see the top of this script, or set PKGTOOL)"
[[ -n "$dotnet" ]] || die "dotnet not found (install the .NET 8 runtime, or set DOTNET)"

tool() { "$dotnet" "$pkgtool" "$@"; }

# A truncated copy fails late and confusingly: check the size the header promises first.
expected=$(od -An -tx1 -j $((0x430)) -N 8 "$pkg" | tr -d ' \n')
actual=$(stat -c %s "$pkg")
((actual >= 16#$expected)) || die "$pkg is truncated ($actual of $((16#$expected)) bytes); copy it again"

# The game's files, into <out>.new first, so a failed run leaves an earlier extraction alone.
rm -rf "$out.new"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
echo "extract-pkg: extracting $pkg (this takes a while)"
if ! nice -n 10 "$dotnet" "$pkgtool" pkg_extract "$pkg" "$out.new" >"$log" 2>&1; then
    if grep -q "is corrupt" "$log"; then
        echo "extract-pkg: retrying with the PKG's new-crypto flag cleared"
        rm -rf "$out.new"
        PKGTOOL_PFS_FLAGS_XOR=2000000000000000 nice -n 10 "$dotnet" "$pkgtool" pkg_extract "$pkg" "$out.new" >"$log" 2>&1 ||
            { cat "$log" >&2; die "extraction failed"; }
    else
        cat "$log" >&2
        die "extraction failed"
    fi
fi
[[ -d "$out.new/uroot" ]] || die "extraction made no uroot folder"

# sce_sys: PKG entries, not part of the file system pkg_extract writes. Encrypted entries
# (license, NP) and the PKG's own tables are skipped. Names: PARAM_SFO -> param.sfo,
# TROPHY__TROPHY00_TRP -> trophy/trophy00.trp, PLAYGO_CHUNK_DAT -> playgo-chunk.dat,
# CHANGEINFO__CHANGEINFO_01_XML -> changeinfo/changeinfo_01.xml.
sce_sys="$out.new/uroot/sce_sys"
tool pkg_listentries "$pkg" | tail -n +2 | while read -r _ _ _ index rest; do
    set -- $rest
    [[ $# -eq 1 ]] || continue # encrypted entries have an Enc column
    name="$1"
    case "$name" in DIGESTS | ENTRY_KEYS | GENERAL_DIGESTS | METAS | ENTRY_NAMES) continue ;; esac
    stem="${name%_*}"
    ext="$(tr 'A-Z' 'a-z' <<<"${name##*_}")"
    file="$(tr 'A-Z_' 'a-z-' <<<"${stem//__//}")"
    [[ "$file" =~ ^(changeinfo/changeinfo)-([0-9]+)$ ]] && file="${BASH_REMATCH[1]}_${BASH_REMATCH[2]}"
    mkdir -p "$(dirname "$sce_sys/$file.$ext")"
    tool pkg_extractentry "$pkg" "$index" "$sce_sys/$file.$ext" >/dev/null
done
[[ -f "$sce_sys/param.sfo" ]] || die "no param.sfo in $pkg"

rm -rf "$out"
mv "$out.new" "$out"
echo "extract-pkg: done: $out/uroot"
