# moonlight-ps4

Unofficial [Moonlight](https://moonlight-stream.org/) port (Sunshine/GameStream
game-streaming client) for jailbroken PS4 (GoldHEN), built on the
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) toolchain.

Full port plan: [PLAN.md](PLAN.md).

**Current version: 1.1.0**

## Status

Playable client for jailbroken PS4 (GoldHEN / FW 9.00 validated):

- [x] Pairing, applist, launch, and H.264 streaming against Sunshine
- [x] Audio (Opus + `sceAudioOut`)
- [x] Software video (FFmpeg) and hardware decode (`libSceVideodec2`, `prefer_hw`)
- [x] DualShock 4 input (`scePad`)
- [x] On-console UI (APPS / SETTINGS + on-screen keyboard) and INI config
- [x] Installable `.pkg` (`CATEGORY=gd`)
- [x] Optional performance overlay and Videodec2 / BGRA tuning knobs

### TODO

- [ ] Keyboard and mouse support
- [ ] mDNS host discovery
- [ ] Native YCbCr/NV12 presentation on `sceVideoOut`

Full port plan and longer backlog: [PLAN.md](PLAN.md). Validation notes: [docs/CONSOLE_VALIDATE.md](docs/CONSOLE_VALIDATE.md).

## Requirements (development)

### Linux

- Linux x86_64 with `clang`, `ld.lld`, `cmake`, `ninja`, `git`, and `curl`
- OpenOrbis toolchain v0.5.4 (default: `~/ps4dev/OpenOrbis/PS4Toolchain`)
- Cross-built FFmpeg for PS4 (H.264 decoder only): `scripts/linux/build_ffmpeg_ps4.sh`
- For `PkgTool.Core`: `libssl.so.1.1` / `libcrypto.so.1.1` in `~/ps4dev/hostlibs/usr/lib`
- PS4 with GoldHEN-compatible firmware (plan validated on **9.00**)
- Host PC running [Sunshine](https://github.com/LizardByte/Sunshine)

### Windows

- Windows 10/11 with [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) (CMake and Ninja ship with it), [LLVM](https://releases.llvm.org/) (`clang` / `ld.lld` at `C:\Program Files\LLVM\bin`), [Git for Windows](https://git-scm.com/) (for Git Bash), and `curl`
- [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) installer. Set the `OO_PS4_TOOLCHAIN_SRC` machine/user env var to the SDK root (e.g. `C:\OpenOrbis\PS4Toolchain`) because that's the only path you need, `scripts\windows\env.bat` and `cmake\openorbis.cmake` read it, nothing is hardcoded. `OO_PS4_TOOLCHAIN` (or the Linux `~/ps4dev/...` fallback) works too and takes precedence.
- Cross-built FFmpeg for PS4 (H.264 decoder only): `scripts/windows/build_ffmpeg_ps4.bat` (wraps `scripts/linux/build_ffmpeg_ps4.sh` under Git Bash "`%USERPROFILE%/ps4dev/ffmpeg-ps4`")
- For `PkgTool.Core`, there's a file called `PkgTool.Core.exe` and needs the .NET runtime installed, no `PS4_HOSTLIBS` / `LD_LIBRARY_PATH` on Windows.
- PS4 with GoldHEN-compatible firmware (plan validated on **9.00**)
- Host PC running [Sunshine](https://github.com/LizardByte/Sunshine)

## Dependencies (third_party)

Vendored libraries live as **git submodules** pinned in [`third_party/DEPS`](third_party/DEPS).
Git only stores the commit SHA for each submodule — not a full copy of upstream.
Orbis-specific fixes are **not** committed inside those repos; they live under
[`patches/`](patches/) and are applied after checkout.

```bash
git clone <this-repo>
scripts/linux/setup_deps.sh    # checkout pins from third_party/DEPS + apply patches/
```

Equivalent manual flow: `git submodule update --init --recursive` then
`scripts/linux/apply_patches.sh`. After patches, `third_party/*` working trees look
dirty — that is expected; commit patch files, not submodule dirt.
On Windows use the `.bat` files under `scripts\windows\` instead.

To bump a dependency on purpose: edit the pin in `third_party/DEPS`, update the
submodule to that commit, refresh patches if needed (`scripts/linux/refresh_patches.sh`),
and commit the new gitlink + DEPS (+ patches).

## Build

```bash
git clone <this-repo>
cd moonlight-ps4
scripts/linux/setup_deps.sh                # pinned third_party + Orbis patches
scripts/linux/build_ffmpeg_ps4.sh          # once (~/ps4dev/ffmpeg-ps4)

# PS4 app + .pkg (sources scripts/linux/env.sh, configures, builds, packages)
scripts/linux/build_pkg.sh
# scripts/linux/build_pkg.sh --clean       # wipe build-ps4/ and reconfigure

# Optional: Linux development CLI (pairing / H.264 dump)
cmake -B build-host -G Ninja && cmake --build build-host
```

Installable artifact: `build-ps4/Moonlight-1.1.0.pkg`
(also `IV0000-MLNT00001_00-MOONLIGHTPS40000.pkg`).

`build_pkg.sh` runs `source scripts/linux/env.sh` itself. Manual CMake equivalent:

```bash
source scripts/linux/env.sh
cmake -B build-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/openorbis.cmake
cmake --build build-ps4
```

### Windows

`cmd`/`cmd.exe`. Every `.sh` in `scripts\linux\` has a `.bat` file with identical behavior in `scripts\windows\`. `scripts\windows\env.bat` and `cmake\openorbis.cmake` read the SDK path from `OO_PS4_TOOLCHAIN` or `OO_PS4_TOOLCHAIN_SRC` (the same SDK root) (again, no paths are hardcoded).

```bat
git clone <this-repo>
cd moonlight-ps4
set OO_PS4_TOOLCHAIN_SRC=C:\OpenOrbis\PS4Toolchain   REM or OO_PS4_TOOLCHAIN
scripts\windows\setup_deps.bat   Rem pinned third_party + Orbis patches
scripts\windows\build_ffmpeg_ps4.bat   Rem once -> %USERPROFILE%\ps4dev\ffmpeg-ps4 (via Git Bash + LLVM clang)

scripts\windows\build_pkg.bat
scripts\windows\build_pkg.bat --clean   Rem wipe build-ps4\ and reconfigure

REM optional, host development cli (pairing / H.264 dump, MSVC)
cmake -S . -B build-host-win -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-host-win --config Release
build-host-win\Release\moonlight-cli.exe
```

Installable artifact: `build-ps4\Moonlight-1.1.0.pkg`.
The Windows packaging uses the OpenOrbis tools under `bin\windows\*.exe`, `PkgTool.Core.exe` targets .NET Core 3.0, and `scripts\windows\env.bat` / `scripts\windows\make_pkg.bat` set `DOTNET_ROLL_FORWARD=LatestMajor` so it runs on your installed .NET instead of requiring the EOL 3.0 runtime.

Manual CMake equivalent (PS4 target on Windows):

```bat
call scripts\windows\env.bat
cmake -S . -B build-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake\openorbis.cmake
cmake --build build-ps4
```

## Usage on PS4

1. Install the `.pkg` (Package Installer / GoldHEN / FTP). The package uses
  `CATEGORY=gd` (required so GoldHEN injects plugins).
2. Optional — YCbCr presentation (`prefer_ycbcr=true`, experimental):
  - Copy `plugin/bin/ycbcr_unlock.prx` → `/data/GoldHEN/plugins/`
  - Add `[MLNT00001]` in `/data/GoldHEN/plugins.ini`
  (see `[plugin/README.md](plugin/README.md)` and `plugin/plugins.ini.example`)
  - GoldHEN → Enable plugins
  - After each reboot, send the kpayload once:
  `nc -w 3 <PS4_IP> 9090 < plugin/bin/ycbcr_kpatch_900.bin`
3. On first launch the app opens the **SETTINGS** menu if no host is set. You can
  also create `/data/moonlight/moonlight.ini` over FTP (see
   `pkg/assets/misc/moonlight.ini`). Optional: `debug_host.txt` with your PC IP.
4. Start Sunshine on the PC. On first pair the app shows a PIN (also written to
  `/data/moonlight/pin.txt`); enter it at `https://<pc>:47990/pin`.
5. Logs on the PC: `nc -ulk 9999` (or enable file log in SETTINGS →
  `/data/moonlight/debug.log`).
6. In the menu: **X/O** start app, **L1/R1** switch APPS/SETTINGS,
  **OPTIONS** quit active Sunshine session, **OPTIONS + Touchpad** (~1 s) exit stream.



### Defaults


| Setting          | Default                                                        |
| ---------------- | -------------------------------------------------------------- |
| Resolution / FPS | 1920×1080 @ 60                                                 |
| Bitrate          | 20 Mbps, H.264                                                 |
| `prefer_hw`      | `true` (Videodec2)                                             |
| `prefer_ycbcr`   | `false` (BGRA path; correct colors, SSE2 multi-thread convert) |
| Title ID         | `MLNT00001`                                                    |


Mean decode/convert/present times are printed every second over UDP.
Validation protocol: `[docs/CONSOLE_VALIDATE.md](docs/CONSOLE_VALIDATE.md)`.

Isolated Videodec2 spike: `videodec2_spike = true` in the INI.

## Host CLI (Linux / Windows)

```bash
./build-host/moonlight-cli pair 192.168.1.100
./build-host/moonlight-cli list 192.168.1.100
./build-host/moonlight-cli stream 192.168.1.100 --res 1280x720 --time 20
./build-host/moonlight-cli quit 192.168.1.100
```

On Windows the same binary is built by the `build-host-win` configure above (`build-host-win\Release\moonlight-cli.exe`) and stores its identity under `%LOCALAPPDATA%\moonlight-ps4` instead of `~/.config/moonlight-ps4`.



## Layout

```
cmake/openorbis.cmake           CMake toolchain for PS4
pkg/                            package assets (icon0, sce modules)
scripts/linux/                  bash scripts (env.sh, build_pkg.sh, .pkg packaging)
scripts/windows/                .bat files of the same steps
src/
  main.c stream.c config.c      orchestration + INI config
  ui/                           on-console menu (APPS / SETTINGS + OSK)
  gamestream/                   pairing/HTTP/XML over mbedTLS
  audio/audio_orbis.c           Opus + sceAudioOut
  video/renderer_videoout.c     YCbCr/NV12 or BGRA presentation (sceVideoOut)
  video/nv12_blit.c             1088→1080 copy (WB_GARLIC / Onion alias)
  video/decoder_ffmpeg.c        H.264 software (FFmpeg)
  video/decoder_orbis.c         H.264 hardware (libSceVideodec2)
  gamestream/sps.c              SPS fixup (num_ref_frames=1)
  input/input_pad.c             DualShock 4
  orbis/                        VideoOut / Videodec2 / net helpers
  host/main_cli.c               Development CLI (Linux / Windows)
plugin/
  ycbcr_unlock/                 GoldHEN PRX (1.63) for YCbCr VideoOut
  ycbcr_kpatch/                 FW 9.00 kernel kpayload (BinLoader)
  kernel_dumper_900/            kernel dump helper for RE
docs/                           console validation + kernel YCbCr RE notes
third_party/
  DEPS                        pinned tags/SHAs (lockfile for setup_deps.sh)
  moonlight-common-c          protocol engine (submodule + Orbis patches)
  mbedtls                     crypto (v3.6.4)
  opus                        audio
  h264bitstream               SPS rewrite
patches/                      Orbis patches applied by scripts/linux/apply_patches.sh (.bat in scripts/windows/)
scripts/linux/                env.sh, setup_deps.sh, apply_patches.sh, refresh_patches.sh, .pkg packaging (.bat twins in scripts/windows/)
```
