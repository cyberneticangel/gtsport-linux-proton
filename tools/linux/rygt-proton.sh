#!/usr/bin/env bash
# rygt-proton.sh: runs RyGT (the Windows build) on Linux through Proton and Vulkan.
#
#   rygt-proton.sh launcher [RyGT folder]        the RyGT launcher (Set up, options, Play)
#   rygt-proton.sh setup <RyGT folder> --game DIR [--update DIR] --modules DIR --fonts DIR
#                                                Set up without the launcher window
#   rygt-proton.sh play [RyGT folder] [args]     GTSport.exe
#   rygt-proton.sh probe [wineprobe.exe]         checks this Proton/GPU for what RyGT needs
#   rygt-proton.sh steam [RyGT folder]           what to enter for a Steam non-Steam game
#
# The RyGT folder defaults to the current directory. Environment:
#   RYGT_PROTON   Proton folder (with the `proton` script) or name, e.g. "Proton - Experimental";
#                 default: the newest Proton Experimental, CachyOS, GE or numbered Proton found
#   RYGT_PREFIX   compatibility data folder (the Wine prefix is its pfx/), default
#                 ~/.local/share/rygt/proton
#   RYGT_UMU=1    use umu-run (umu-launcher) instead of calling Proton directly
#   RYGT_USERPTR=1  experimental: load rygt-userptr.so (see rygt-userptr.c), which lets the
#                 direct renderer import guest memory on AMD GPUs under Proton (off by default: the
#                 imports work, but cost more than they save once the game streams memory)
# Anything else in the environment (SHADGT_*, RYGT_*, PROTON_*, DXVK_*, WINEDEBUG) reaches the game.
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
prefix="${RYGT_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/rygt/proton}"

die() { echo "rygt-proton: $*" >&2; exit 1; }
note() { echo "rygt-proton: $*" >&2; }

steam_root() {
    local dir
    for dir in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
        if [[ -d "$dir" ]]; then
            (cd "$dir" && pwd -P)
            return
        fi
    done
}

# Every Proton install: Steam's own (all library folders) and compatibilitytools.d.
proton_candidates() {
    local root library
    root="$(steam_root)"
    [[ -n "$root" ]] || return 0
    local libraries=("$root")
    if [[ -f "$root/steamapps/libraryfolders.vdf" ]]; then
        while IFS= read -r library; do libraries+=("$library"); done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"/\1/p' "$root/steamapps/libraryfolders.vdf")
    fi
    for library in "${libraries[@]}"; do
        for dir in "$library"/steamapps/common/Proton*/; do [[ -x "$dir/proton" ]] && echo "${dir%/}"; done
    done
    for dir in "$root"/compatibilitytools.d/*/ "$HOME"/.steam/root/compatibilitytools.d/*/ /usr/share/steam/compatibilitytools.d/*/; do
        [[ -x "$dir/proton" ]] && echo "${dir%/}"
    done
    return 0
}

find_proton() {
    local candidates wanted dir
    mapfile -t candidates < <(proton_candidates | awk '!seen[$0]++')
    if [[ -n "${RYGT_PROTON:-}" ]]; then
        [[ -x "$RYGT_PROTON/proton" ]] && { echo "$RYGT_PROTON"; return; }
        for dir in "${candidates[@]}"; do
            [[ "$(basename "$dir")" == "$RYGT_PROTON" ]] && { echo "$dir"; return; }
        done
        die "RYGT_PROTON=$RYGT_PROTON is not a Proton folder or an installed Proton name"
    fi
    # Newest first: Experimental, then CachyOS/GE builds, then the highest numbered Proton.
    for wanted in "Proton - Experimental" "proton-cachyos" "Proton-CachyOS" "GE-Proton"; do
        for dir in "${candidates[@]}"; do
            [[ "$(basename "$dir")" == "$wanted"* ]] && { echo "$dir"; return; }
        done
    done
    for dir in "${candidates[@]}"; do echo "$dir"; done | grep -E '/Proton [0-9]+' | sort -t' ' -k2 -V | tail -n1 | grep . && return
    die "no Proton found; install Proton Experimental in Steam or set RYGT_PROTON"
}

check_system() {
    local count
    count="$(cat /proc/sys/vm/max_map_count 2>/dev/null || echo 0)"
    if ((count < 1048576)); then
        note "vm.max_map_count is $count; RyGT maps guest memory in many small views and needs more:"
        note "  sudo sysctl -w vm.max_map_count=1048576   (and add it to /etc/sysctl.d to keep it)"
    fi
}

has_amd_gpu() {
    grep -qx 0x1002 /sys/class/drm/card*/device/vendor 2>/dev/null
}

# rygt-userptr.so, built from rygt-userptr.c next to this script into the RyGT data folder
# (again when the source is newer). Prints its path, or nothing when it can't be built.
userptr_library() {
    local source="$here/rygt-userptr.c" library="${prefix%/*}/rygt-userptr.so" cc
    [[ -f "$source" ]] || return 0
    if [[ ! -f "$library" || "$source" -nt "$library" ]]; then
        cc="$(command -v cc || command -v gcc || command -v clang || true)"
        if [[ -z "$cc" ]]; then
            note "no C compiler, so the direct renderer can't import guest memory on AMD (install gcc)"
            return 0
        fi
        mkdir -p "${library%/*}"
        "$cc" -O2 -shared -fPIC -o "$library" "$source" -ldl || { note "could not build $library"; return 0; }
    fi
    echo "$library"
}

# RYGT_USERPTR=1: loads rygt-userptr.so into the game (experimental, AMD GPUs only).
userptr_preload() {
    local library
    [[ "${RYGT_USERPTR:-0}" == 1 ]] || return 0
    has_amd_gpu || { note "RYGT_USERPTR=1 only matters on AMD GPUs; not loading rygt-userptr.so"; return 0; }
    library="$(userptr_library)"
    [[ -n "$library" ]] && export LD_PRELOAD="$library${LD_PRELOAD:+:$LD_PRELOAD}"
    return 0
}

# Runs a Windows program in the RyGT prefix and waits for it.
run_windows() {
    local exe="$1"
    shift
    mkdir -p "$prefix"
    userptr_preload
    if [[ "${RYGT_UMU:-0}" == 1 ]]; then
        command -v umu-run >/dev/null || die "RYGT_UMU=1 but umu-run is not installed"
        local proton=""
        [[ -n "${RYGT_PROTON:-}" ]] && proton="$(find_proton)"
        WINEPREFIX="$prefix/pfx" GAMEID="${GAMEID:-umu-default}" PROTONPATH="${proton:-GE-Proton}" umu-run "$exe" "$@"
        return
    fi
    local proton root
    proton="$(find_proton)"
    root="$(steam_root)"
    [[ -n "$root" ]] || die "Steam is not installed (Proton needs its client files); set RYGT_UMU=1 to use umu-run instead"
    note "using $(basename "$proton")"
    STEAM_COMPAT_CLIENT_INSTALL_PATH="$root" STEAM_COMPAT_DATA_PATH="$prefix" \
        "$proton/proton" waitforexitandrun "$exe" "$@"
}

rygt_folder() {
    local folder="${1:-$PWD}"
    folder="$(cd "$folder" 2>/dev/null && pwd -P)" || die "no such folder: ${1:-$PWD}"
    [[ -f "$folder/RyGT.exe" || -f "$folder/GTSport.exe" ]] || die "$folder has neither RyGT.exe nor GTSport.exe (unzip a RyGT release there)"
    echo "$folder"
}

# A Linux path as Wine sees it (Z: is /).
windows_path() {
    local path
    path="$(cd "$1" 2>/dev/null && pwd -P)" || die "no such folder: $1"
    echo "Z:${path//\//\\}"
}

cmd_launcher() {
    local folder
    folder="$(rygt_folder "${1:-}")"
    check_system
    cd "$folder"
    run_windows "$folder/RyGT.exe"
}

cmd_setup() {
    [[ $# -ge 1 ]] || die "usage: setup <RyGT folder> --game DIR [--update DIR] --modules DIR --fonts DIR"
    local folder game="" update="" modules="" fonts=""
    folder="$(rygt_folder "$1")"
    shift
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --game) game="$2"; shift 2 ;;
            --update) update="$2"; shift 2 ;;
            --modules) modules="$2"; shift 2 ;;
            --fonts) fonts="$2"; shift 2 ;;
            *) die "setup: unknown option $1" ;;
        esac
    done
    [[ -n "$game" && -n "$modules" && -n "$fonts" ]] || die "setup needs --game, --modules and --fonts"
    [[ -f "$folder/RyGT.exe" ]] || die "$folder/RyGT.exe is missing"
    {
        echo "game=$(windows_path "$game")"
        echo "update=$( [[ -n "$update" ]] && windows_path "$update")"
        echo "modules=$(windows_path "$modules")"
        echo "fonts=$(windows_path "$fonts")"
    } >"$folder/RyGT.ini"
    link_app0 "$folder" "$game" "$update"
    cd "$folder"
    local status=0
    RYGT_APP0_LINKED=1 run_windows "$folder/RyGT.exe" --setup || status=$?
    [[ -f "$folder/setup.log" ]] && cat "$folder/setup.log"
    ((status == 0)) || die "Set up failed (exit code $status); see above"
    native_adcpatch "$folder" "$game" "$update"
}

# app0 as Set up makes it (the base game's files and folders, the update's over them, without eboot.bin
# and sce_module), but with real Linux symlinks: every Proton follows those, while Proton 10 and older
# cannot open the links Wine makes itself. RyGT.exe --setup then leaves app0 alone (RYGT_APP0_LINKED).
link_app0() {
    local folder="$1" app0="$1/app0" source entry name
    mkdir -p "$app0/sce_module"
    for source in "$2" ${3:+"$3"}; do
        source="$(cd "$source" && pwd -P)"
        for entry in "$source"/*; do
            name="$(basename "$entry")"
            [[ "$name" == eboot.bin || "$name" == sce_module ]] && continue
            # A real folder there is not one of Set up's links: leave it to the player.
            if [[ -d "$app0/$name" && ! -L "$app0/$name" ]]; then
                rmdir "$app0/$name" 2>/dev/null || die "$app0/$name is a folder with files in it; move it away and run setup again"
            fi
            ln -sfn "$entry" "$app0/$name"
        done
    done
    note "linked the game files into $app0"
}

# adcpatch.exe is a self-contained .NET 10 program, which can fail under Wine. A Linux build
# (dotnet publish -r linux-x64, saved as tools/adcpatch/adcpatch next to adcpatch.exe) makes the
# offline livery scripts natively when Set up could not.
native_adcpatch() {
    local folder="$1" game="$2" update="$3"
    local tool="$folder/tools/adcpatch/adcpatch" scripts="$folder/patches/scripts"
    local missing=0 name
    for name in UI_KIT.ADC LIVERY2.ADC THUMBNAIL.ADC GALLERY.ADC GRIM.ADC; do
        [[ -f "$scripts/$name" ]] || missing=1
    done
    ((missing)) || return 0
    if [[ ! -x "$tool" ]]; then
        note "the offline livery scripts were not made (adcpatch.exe failed under Wine?)."
        note "  for them, build tools/adcpatch for linux-x64 (see tools/adcpatch/README.txt) as $tool and run setup again"
        return 0
    fi
    note "making the offline livery scripts with the Linux adcpatch"
    mkdir -p "$scripts"
    (cd "$(dirname "$tool")" && "$tool" all "$game" "${update:-}" "$scripts")
    # The launcher keeps the hash of the adcpatch.exe that made the scripts, so it does not redo them.
    if [[ -f "$folder/tools/adcpatch/adcpatch.exe" ]]; then
        sha256sum "$folder/tools/adcpatch/adcpatch.exe" | awk '{print toupper($1)}' >"$scripts/made-by.txt"
    fi
}

cmd_play() {
    local folder
    folder="$(rygt_folder "${1:-}")"
    [[ $# -gt 0 ]] && shift
    [[ -f "$folder/GTSport.exe" ]] || die "$folder/GTSport.exe is missing; run setup (or Set up in the launcher) first"
    check_system
    cd "$folder"
    run_windows "$folder/GTSport.exe" "$@"
}

cmd_probe() {
    local exe="${1:-}"
    if [[ -z "$exe" ]]; then
        exe="$here/../wineprobe/wineprobe.exe"
        [[ -f "$exe" ]] || "$here/../wineprobe/build.sh" >/dev/null || die "could not build wineprobe (install mingw-w64-gcc)"
    fi
    check_system
    exe="$(cd "$(dirname "$exe")" && pwd -P)/$(basename "$exe")"
    local log="${exe%.exe}.log" status=0
    rm -f "$log"
    run_windows "$exe" >/dev/null || status=$?
    [[ -f "$log" ]] && cat "$log"
    return "$status"
}

cmd_steam() {
    local folder proton
    folder="$(rygt_folder "${1:-}")"
    proton="$(find_proton)"
    local launch="%command%" library
    # RYGT_USERPTR=1 (experimental, AMD): the game needs rygt-userptr.so too (see rygt-userptr.c).
    if [[ "${RYGT_USERPTR:-0}" == 1 ]] && has_amd_gpu; then
        library="$(userptr_library)"
        [[ -n "$library" ]] && launch="LD_PRELOAD=$library %command%"
    fi
    cat <<EOF
In Steam: Games > Add a Non-Steam Game to My Library > Browse, pick
    $folder/GTSport.exe        (or RyGT.exe for the launcher)
then in the new entry's Properties:
    Shortcut > START IN:        "$folder"
    Shortcut > LAUNCH OPTIONS:  $launch
                                (e.g. SHADGT_DIRECT=0 %command% to test the copying renderer)
    Compatibility:              tick "Force the use of a specific Steam Play compatibility tool"
                                and pick $(basename "$proton")
Set up first (rygt-proton.sh setup, or the launcher under Proton Experimental): Steam gives the
entry its own Wine prefix, but the converted game and its settings are in the RyGT folder.
EOF
}

case "${1:-}" in
    launcher) shift; cmd_launcher "$@" ;;
    setup) shift; cmd_setup "$@" ;;
    play) shift; cmd_play "$@" ;;
    probe) shift; cmd_probe "$@" ;;
    steam) shift; cmd_steam "$@" ;;
    *) sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
