# gtsport-linux-proton

Gran Turismo Sport (PS4) on Linux, through [RyGT](https://github.com/mlgprorektm8/RyGT) and Proton.

RyGT turns GT Sport's executable into a Windows program (`GTSport.exe`) that runs the game code
directly on your CPU, with a Vulkan renderer ([shadGT](https://github.com/mlgprorektm8/shadGT/tree/ryty-gpu))
standing in for the PS4's GPU. This fork adds what it takes to set that up and run it on Linux:

- `tools/linux/rygt-proton.sh`: set up, play and check RyGT in a Proton prefix of its own;
- `tools/linux/extract-pkg.sh`: extracts your PKGs into folders Set up accepts, with fixes for
  two LibOrbisPkg bugs that otherwise corrupt the game files or stop the 1.69 update;
- `tools/linux/patch-libkernel-tz.py`: lets current RyGT builds start under Proton;
- `tools/wineprobe`: checks your Proton and GPU for what RyGT needs, without the game;
- runtime and loader changes for Wine (guest thread stacks, Wine detection), and support for the
  European release **CUSA02168** next to the US one (CUSA03220).

The original README is in [README.upstream.md](README.upstream.md). The details of everything below
are in [docs/gt-sport/LINUX.md](docs/gt-sport/LINUX.md).

No game files, PS4 firmware files or fonts are included. You need your own GT Sport (base game
and update 1.69) and your own PS4 firmware's modules and fonts.

## What you need

| | |
| --- | --- |
| Game | GT Sport CUSA03220 (US) or CUSA02168 (EU): the base game PKG and the 1.69 update PKG, or folders already extracted |
| Firmware | Decrypted system modules (`libSceFont`, `libSceFontFt`, `libSceFreeTypeOt`, `libSceJson2`, `libSceNgs2`, and `libSceJpegDec`/`libScePngEnc` if you have them; `.sprx` or `.prx`) and the `font` and `font2` folders from `/system/common` |
| RyGT | A RyGT release zip (Windows build) from [Releases](https://github.com/mlgprorektm8/RyGT/releases) |
| Proton | Steam with **Proton Experimental** (Proton-CachyOS or GE builds on Wine 11 also work) |
| GPU | Vulkan with `VK_EXT_external_memory_host` (current Mesa RADV or NVIDIA) |
| System | `vm.max_map_count` of at least 1048576 (the default on Arch, CachyOS, Fedora, SteamOS, Ubuntu 24.04+) |
| Tools | `gcc`, Python 3, the .NET 8 SDK (PKG extraction); Mono (only to build the loader); `mingw-w64-gcc` (only for the probe) |

About 100 GB of free space for the extracted game, plus the PKGs while you extract them.

## Build

The game's runtime and renderer are Windows programs, built on Windows as described in
[docs/user/BUILDING.md](docs/user/BUILDING.md) and [renderer/README.md](renderer/README.md); use a
release zip for them. The Linux-side pieces build here:

**PKG extraction tool** (LibOrbisPkg's PkgTool, with this fork's fixes):

```sh
git clone https://github.com/maxton/LibOrbisPkg && cd LibOrbisPkg
git checkout 6434772
git apply ~/gtsport-linux-proton/tools/linux/liborbispkg-linux.patch
dotnet publish PkgTool.Core/PkgTool.Core.csproj -c Release -o ~/.local/share/rygt/pkgtool
```

**Loader** (`RyGT.exe`, the launcher and Set up), when you want this fork's version, for example for
the European release. Needs Mono:

```sh
loader/build.sh ~/Games/RyGT        # replaces the release's RyGT.exe; keep a copy if you like
```

**Offline livery scripts tool** (optional; the release's `adcpatch.exe` normally works under
Proton Experimental): see [tools/adcpatch/README.txt](tools/adcpatch/README.txt), publish it with
`-r linux-x64` and save it as `tools/adcpatch/adcpatch` in the RyGT folder.

**wineprobe** builds by itself the first time you run `rygt-proton.sh probe`.

## Use

**1. Extract the game.** Once per PKG; each takes a few minutes and writes `<folder>/uroot`:

```sh
tools/linux/extract-pkg.sh base.pkg   ~/PS4/CUSA02168
tools/linux/extract-pkg.sh update.pkg ~/PS4/CUSA02168-patch
```

It checks that the PKG is complete first (a truncated copy is the most common problem), and also
writes `sce_sys` (`param.sfo` and the rest), which the game needs and PkgTool leaves out.

**2. Unpack RyGT** and make it start under Proton:

```sh
mkdir -p ~/Games && cd ~/Games && unzip GTSport-Ryty-shadGT-*.zip && mv GTSport-Ryty-shadGT-*/ RyGT
python3 ~/gtsport-linux-proton/tools/linux/patch-libkernel-tz.py ~/Games/RyGT
```

The patch script does nothing on builds that don't need it.

**3. Set up** (converts the game, links the game files, copies the fonts, makes the offline
livery scripts):

```sh
tools/linux/rygt-proton.sh setup ~/Games/RyGT \
    --game ~/PS4/CUSA02168/uroot --update ~/PS4/CUSA02168-patch/uroot \
    --modules ~/PS4/firmware/sys_modules --fonts ~/PS4/firmware/fonts
```

`--fonts` is the folder holding `font` and `font2`. Set up's log is in `~/Games/RyGT/setup.log`.

**4. Play:**

```sh
tools/linux/rygt-proton.sh play ~/Games/RyGT
tools/linux/rygt-proton.sh launcher ~/Games/RyGT   # the launcher window: options, controls, Play
tools/linux/rygt-proton.sh steam ~/Games/RyGT      # what to enter to add it to Steam instead
```

The game writes `stdout.log` and `stderr.log` in the RyGT folder. Every 10 seconds `stdout.log`
gets a line like `DIAG-046 frames in 10.0 s: ... (60.0 fps)`, and slow frames get a `hitch` line
saying where the time went.

## Performance

- **The first hours stutter.** The renderer builds each new GPU pipeline the first time the game
  needs it, and the release has no prepared shader list. Everything it builds is kept in
  `user/cache`, and with "Precompile shaders" on (the default), it is built again on the splash
  screen at the next start instead of during play. In the launcher, set **When new shaders
  compile** to **Keep going** so the game doesn't slow down while a shader builds (in races it
  never does).
- **Busy scenes are slower than on Windows, especially on AMD.** Under Proton the direct renderer
  can't import game memory on AMD GPUs, so it copies buffers and write-protects their pages, and
  every protection is slow under Wine. LINUX.md explains why, and what the experimental
  `RYGT_USERPTR=1` does about it (it is off because, for now, it makes long races slower).
- **Settings that help:** untick **Wet-road reflections** for much more speed in rain, tick
  **Rear-view mirror at half rate**, and use `powerprofilesctl set performance` while playing.

## Troubleshooting

| What you see | Why, and what to do |
| --- | --- |
| The game closes right after the window opens; `play` output has `unimplemented function KERNEL32.dll.SystemTimeToTzSpecificLocalTimeEx` | Run `patch-libkernel-tz.py` on the RyGT folder (step 2) |
| `stderr.log` ends with `neither param.json nor param.sfo found` | The game folders have no `sce_sys/param.sfo`: extract with `extract-pkg.sh`, which writes it |
| `extract-pkg.sh`: `... is truncated` | Copy the PKG again; compare its size with the original |
| Set up: "the offline livery scripts could not be made", or adcpatch crashes reading `gt.idx` | The game files were extracted with an unpatched PkgTool and have holes; extract again with the patched one |
| Everything is slow and the disk is busy | Another extraction or Set up is still running; both are I/O heavy |
| Anything else | Run `tools/linux/rygt-proton.sh probe` and see [LINUX.md](docs/gt-sport/LINUX.md) |

## License

GPL-2.0, as RyGT and Ryty; see [LICENSE](LICENSE). Third-party parts keep their own licenses.
