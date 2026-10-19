# GT Sport on Linux (Proton)

RyGT's Windows build runs on Linux through Proton: Wine runs `GTSport.exe` and the runtime, and the
renderer's Vulkan calls go through winevulkan to the Linux driver (Mesa RADV, NVIDIA, ...). You use
the same release, game dump and firmware files as on Windows.

## Requirements

- **Proton Experimental** (Proton 11) is recommended. Proton-CachyOS and GE builds based on Wine 11
  work too. Proton 9 and 10 pass the same memory and Vulkan checks (see wineprobe below), but their
  timed waits are less precise and the launcher's Set up can't link the game files there; with them,
  set up with `rygt-proton.sh setup` (below).
- A Vulkan driver with `VK_EXT_external_memory_host`. Current Mesa RADV and NVIDIA drivers have it.
- `vm.max_map_count` of at least 1048576. Arch, CachyOS, Fedora, SteamOS and Ubuntu 24.04 and later
  already set this; elsewhere use `sudo sysctl -w vm.max_map_count=1048576` and add it to
  `/etc/sysctl.d/` to keep it.

## Setting up with the script

`tools/linux/rygt-proton.sh` (in this repository) finds your Steam Proton installs and keeps a Wine
prefix of its own in `~/.local/share/rygt/proton`.

```sh
# 1. Unzip a RyGT release, e.g. to ~/Games/RyGT, then:
rygt-proton.sh setup ~/Games/RyGT \
    --game ~/PS4/CUSA03220 --update ~/PS4/CUSA03220-patch \
    --modules ~/PS4/firmware/sce_module --fonts ~/PS4/firmware/font
# 2. Play:
rygt-proton.sh play ~/Games/RyGT
# The launcher window (options, controls, Play), if you prefer it:
rygt-proton.sh launcher ~/Games/RyGT
```

`setup` runs the launcher's own Set up without its window. One step is done differently: the game's
files are linked into `app0` with real Linux symlinks, so the game, its update and the RyGT folder
can be on any drives and any Proton version works.

To pick a Proton version, use `RYGT_PROTON="Proton 10.0"` or `RYGT_PROTON=/path/to/proton-folder`.
`RYGT_UMU=1` uses umu-launcher instead of Steam's Proton. Other variables in your environment
(`SHADGT_*`, `RYGT_USER_NAME`, `WINEDEBUG`, `PROTON_*`) reach the game.

## Setting up with the launcher

The launcher (`RyGT.exe`) works under Proton Experimental: run `rygt-proton.sh launcher`, or add
`RyGT.exe` to Steam as below. Set up links the game's files with hard links when the RyGT folder is
on the same filesystem as the game, and with Wine symlinks otherwise. Only Proton Experimental (Wine
11) can open those symlinks; with an older Proton, use the script.

## Steam (non-Steam game)

`rygt-proton.sh steam ~/Games/RyGT` prints these values for your folder:

1. In Steam: **Games > Add a Non-Steam Game to My Library > Browse** and pick `GTSport.exe` (or
   `RyGT.exe` for the launcher) in the RyGT folder.
2. In the new entry's **Properties > Shortcut**, set **Start in** to the RyGT folder. Under
   **Launch options**, `%command%` is enough; prefix variables as needed, e.g.
   `SHADGT_DIRECT=0 %command%`.
3. Under **Compatibility**, tick **Force the use of a specific Steam Play compatibility tool** and
   pick Proton Experimental.

Set up first with the script, or with the launcher entry. Steam gives the entry its own Wine prefix,
but the converted game and its settings are in the RyGT folder.

## What is different under Proton

- **AMD GPUs and the direct renderer.** The renderer normally lets the GPU read game memory in place,
  through an import (`VK_EXT_external_memory_host`). Under Wine, the game's graphics memory is a
  shared file mapping. RADV turns an import into an amdgpu userptr, and libdrm_amdgpu always asks
  the kernel for anonymous memory only (`AMDGPU_GEM_USERPTR_ANONONLY`), so every import is refused
  and the renderer logs `D1: ... import refused`. It then copies the buffers and write-protects
  their pages to notice changes, as `SHADGT_DIRECT=0` does; each protection is a slow trip through
  Wine, so busy scenes (races) spend much of a frame on them.

  `tools/linux/rygt-userptr.c` clears that one flag on that one ioctl (`LD_PRELOAD`), and the kernel
  then imports the shared mappings: the wineprobe import checks pass and the protections almost
  disappear (about 1,100 to 18 per slow frame). It is **off by default** because it does not pay
  off yet: amdgpu throws a userptr import away whenever a page in it changes protection or is
  remapped, and checks it again on the next GPU submission. The runtime does that constantly, so
  as the game streams more memory in (2.3 GB of imports in a race), the GPU submission thread
  ends up busy re-checking imports and races fall to a few fps. Making it pay off needs the
  renderer to import only memory the runtime never protects or remaps. To try it:
  `RYGT_USERPTR=1 rygt-proton.sh play`. The probe below shows whether your driver takes the
  imports.
- **NVIDIA DLSS** (the separate plugin) has not been tested under Proton. Proton gives Vulkan games
  NVIDIA's DLSS through dxvk-nvapi with `PROTON_ENABLE_NVAPI=1`. DLSS frame generation goes through
  Streamline, which is less likely to work. AMD FSR 3.1 upscaling and frame generation don't depend
  on the vendor.
- **Discord Rich Presence** talks to Discord's Windows pipe, which a Linux Discord doesn't serve
  without an IPC bridge (for example wine-discord-ipc-bridge). Without one, the option does nothing.
- **The offline livery scripts** are made by `tools\adcpatch\adcpatch.exe`, a .NET 10 program that
  may fail under Wine. Set up only logs a note and goes on; liveries then don't apply offline. A
  `linux-x64` build of adcpatch (see `tools/adcpatch/README.txt`, `dotnet publish -r linux-x64`),
  saved as `tools/adcpatch/adcpatch` next to `adcpatch.exe`, is used by `rygt-proton.sh setup`.

## Checking a setup: wineprobe

`rygt-proton.sh probe` builds (with `mingw-w64-gcc`) and runs `tools/wineprobe/wineprobe.exe` in the
same Proton. Without a game dump, it checks what RyGT needs from Wine and the GPU:

- the placeholder memory APIs and the fixed guest address windows;
- shared views and aliases;
- write-fault handling;
- guest thread stacks;
- timer precision;
- Vulkan imports of private and shared memory, with GPU copies.

Each line says PASS, WARN or FAIL. On an RX 7800 XT (RADV) with Proton Experimental, everything passes except:

- `guest stacks, commit size (current)`: this is how Windows does it. RyGT's runtime uses the
  reservation method under Wine, which passes.
- `import section view` and `import read-write alias view`: the AMD limitation above. With
  `RYGT_USERPTR=1 rygt-proton.sh probe` they pass.

The results are also saved to `wineprobe.log`.

## Troubleshooting

- The game writes `stdout.log` and `stderr.log` in the RyGT folder. The renderer's `D1:` lines show
  whether the GPU imports game memory.
- `WINEDEBUG=+virtual,+seh rygt-proton.sh play` logs Wine's memory and exception handling (very
  verbose; Proton writes it to the terminal).
- `PROTON_LOG=1` writes Proton's log to `~/steam-<appid>.log` (`steam-0.log` for the script).
