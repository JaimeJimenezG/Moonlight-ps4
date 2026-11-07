# GoldHEN plugin: `ycbcr_unlock` 1.63

Unlocks **userspace** validation of `sceVideoOutRegisterBuffers` with format
`YCbCr420_BT709` (`0x08322200`) on FW 9.00 homebrew. Without the plugin,
registration fails with `0x80290003` (`INVALID_PIXEL_FORMAT`).

For the **kernel** ioctl reject (`ret=-1` → `0x80290001` with the same layout
as BGRA OK) you also need the kpayload
[`ycbcr_kpatch`](ycbcr_kpatch/README.md) via BinLoader `:9090` **once per
boot**. RE: [`docs/KERNEL_YCBCR_RE.md`](../docs/KERNEL_YCBCR_RE.md).

Current Moonlight package: **1.1.0** (`Moonlight-1.1.0.pkg`), `CATEGORY=gd`.
App defaults: `prefer_hw=true`, `prefer_ycbcr=false` (BGRA path). Enable YCbCr
from the on-console Settings menu or `moonlight.ini` after kpayload + plugin.

## What it does (1.63)

1. In `plugin_load` (**only**): `HOOK32(sceVideoOutRegisterBuffers)` (+ SubmitFlip
   hook) — no `fopen`, no jailbreak, no patches (avoids SIGBUS before `main`)
2. On the **first** YCbCr `RegisterBuffers`: jailbreak, resolve VideoOut,
   FW 9.00 patches, YCC privilege, marker/diag
3. **Ioctl fixup** (historical `FIX_MODE=2` in 1.43–1.48): completes
   `addr[1]`/`addr[2]` at `+0x18`/`+0x20` and forces `+0x08=2` (like BGRA OK;
   the YCbCr gate left it at 0). Magic `a5a5` at `+0x00` is preserved.
   From **1.49+** / current **1.63**, production default is `FIX_MODE=0`
   (natural tag; kpatch + `BIT_YCC_EXT`)
4. Marker `/data/moonlight/ycbcr_unlock.loaded` + diag `/data/moonlight/vo_diag.txt`

## Bisect finding (console)

M1–M5 and ref all fail the same with `0x80290001`: the patches are **not** the
cause. After ptr fill (1.40–1.42) the ioctl still returned `01`. Diff
BGRA(OK) vs YCbCr(fail): both have `a5a5` and 3 VAs; **only BGRA has `+0x08=2`**.
Userspace fix = Path C + tag `+0x08=2`. Spy 1.46: same layout and same VAs →
YCbCr still `ret=-1` → **kpayload** required.

## Build

```bash
export OO_PS4_TOOLCHAIN=$HOME/ps4dev/OpenOrbis/PS4Toolchain
export GOLDHEN_SDK=$HOME/ps4dev/GoldHEN_Plugins_SDK
make -C plugin/ycbcr_unlock
# → plugin/bin/ycbcr_unlock.prx   (production; FIX_MODE=0 default since 1.49)

# Kernel kpayload (BinLoader):
export PS4SDK=/path/to/ps4-payload-sdk   # built libPS4.a
make -C plugin/ycbcr_kpatch
# → plugin/bin/ycbcr_kpatch_900.bin

# PRX diagnostic / fix variants:
make -C plugin/ycbcr_unlock bisect   # ycbcr_unlock_m1.prx .. _m5.prx (FIX=0)
make -C plugin/ycbcr_unlock FIX=1    # ycbcr_unlock_fix1.prx (AddBuffer)
make -C plugin/ycbcr_unlock FIX=2    # ycbcr_unlock_fix2.prx (explicit fixup)
```

Full validation procedure:
[`docs/CONSOLE_VALIDATE.md`](../docs/CONSOLE_VALIDATE.md) (steps 0b, 1b–1d).

## Install (FTP)

1. Copy `plugin/bin/ycbcr_unlock.prx` → `/data/GoldHEN/plugins/`
2. In `/data/GoldHEN/plugins.ini` (see `plugins.ini.example`):

```ini
[MLNT00001]
/data/GoldHEN/plugins/ycbcr_unlock.prx
```

3. GoldHEN → **Enable plugins**
4. The Moonlight pkg must be **`CATEGORY=gd`** (game). With `gde` (MiniApp)
   GoldHEN does **not** inject plugins. `scripts/linux/make_pkg.sh` already uses `gd`.
5. After every reboot: `nc -w 3 <PS4_IP> 9090 < plugin/bin/ycbcr_kpatch_900.bin`
   (notification `ycbcr_kpatch OK`). **Panic risk** if pattern/offset is wrong;
   the patch does not survive reboot.

### Enable / disable

GoldHEN loads the PRX **only** if listed in `plugins.ini` (title-id section or
`[default]`). Remove those lines to disable the plugin without deleting the
`.prx`.

## Known crash (SIGBUS on launch)

If Moonlight crashes **before** writing `debug.log` with the plugin enabled:

| Klog signal | Meaning |
|-------------|---------|
| `signal: 10 (SIGBUS)` + `general protection fault` | Fault in the process |
| `rax/rsi = 0x601` | Flags `O_WRONLY\|O_CREAT\|O_TRUNC` → `fopen(..., "w")` |
| Libs: `.../ycbcr_unlock.prx` | PRX is injected |
| No `[ycbcr_unlock]` lines in klog | Died before `notify()` |

Cause: older PRX ran jailbreak + `fopen(/data/moonlight/...)` +
`libSceVideoOut` patches in `plugin_load`, which GoldHEN calls **before**
`main()`. On FW 9.00 that ends in SIGBUS inside libc.

Current PRX only installs the hook on load; the rest is deferred.

## Verification

Launch Moonlight and start a stream (with kpayload already applied):

- klog/notify: `ycbcr_kpatch OK` (boot); `[ycbcr_unlock] …` on present
- Notification: `ycbcr 1.63`
- File `/data/moonlight/ycbcr_unlock.loaded` (after first YCbCr)
- `vo_diag.txt`: `ioctl ret=0` on YCbCr; with 1.49+ / 1.63 default, natural `+0x08`
  (do not force 2)
- In logs: `present: YCbCr try … => 0x00000000`, `flip probe … => 0`, `present_mode=YCbCr`
- Plugin 1.49+ / 1.63: klog `SubmitFlip idx=… => 0x00000000` (previously failed `0x80290001`)
