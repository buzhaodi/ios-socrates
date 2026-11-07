# ios-socrates

**iOS 16 running natively on the Xiaomi Redmi K60 Pro** (`socrates`, Snapdragon 8 Gen 2 / SM8550) — the unmodified iOS 16.7.16 (20H392) system of the iPhone X on XNU (Apple's open-source kernel), patched for this hardware. No emulator, no Android underneath. The port is a fork of the [ios-apollo](https://github.com/aTechGeekDev/ios-apollo) recipe (Mi 10T / SM8250), proven portable by [ios-beryllium](https://github.com/LakoMoor/ios-beryllium) (Poco F1 / SDM845).

**Repo**: `https://github.com/buzhaodi/ios-socrates` — the `socrates` branch is the active campaign; it is pushed to both `socrates` and `main`.

> [!WARNING]
> This is an experiment, not a daily-use phone OS. Most of Apple's security model is switched off, and installing the iOS root **erases Android's `userdata` partition**. Every test boot on the phone is a temporary `fastboot boot` — nothing is flashed.

---

## Status

XNU boots on the phone; the UFS root path is being brought up. Full plan, device facts and the round-by-round campaign log: [`PORTING-SOCRATES.md`](PORTING-SOCRATES.md).

| # | Milestone | Status | Evidence |
| --- | --- | --- | --- |
| 1 | Shim boots, draws on the panel | ✅ Done | `fastboot boot` → shim probe report on the 1440×3200 panel |
| 2 | Internal UFS read | ✅ Done | 19-round bring-up: TUR rc=0, 6 LUNs, the GPT partition table — the wake-up recipe is in `PORTING-SOCRATES.md` |
| 3 | XNU reaches fbcon | ✅ Done | On hardware: the full SWEET driver set, `AppleCLCD 1440x1024 stride 5760`, live 60 Hz vsync — GICv3, timer, display all work on the real SM8550 |
| 4 | iOS root, launchd → SpringBoard | 🚧 In progress | Kernel boots with `rd=ufsroot0`; the kernel-side UFS wake (patch 0187) is converging round by round — see the campaign log |
| 5-9 | Touch / buttons+backlight / GPU / Wi-Fi / audio | ❌ Not started | The driver milestones open after M4 |

## How it boots

```
Xiaomi bootloader (unlocked)
  └─ fastboot boot xnu-boot.img      temporary: nothing is flashed to boot
     └─ shim                        loads XNU + the Apple device tree, wakes the UFS
        └─ XNU kernel (SWEET patches)  187-patch series on xnu-8796.141.3
           └─ iOS root (HFS+)           in the userdata partition
              └─ launchd → backboardd → SpringBoard
```

## Build everything (from scratch)

Mac with Xcode command-line tools, Homebrew `clang/llvm`, and `gh` logged in. Python 3 needed. Kernel CI runs on GitHub Actions (macOS runners) — a local kernel build is not required; the kernel binary is downloaded from the CI artifact.

```bash
# 0. one-time setup
brew install llvm gh
gh auth login

# 1. the device tree (Apple-format ADT; socrates constants inside)
python3 tools/mkadt.py shim/out/socrates.adt

# 2. the kernel — push a change to xnu/patches/** and the CI builds it:
git push origin socrates:main
gh run watch   # the xnu workflow, ~10 min
gh run download <run-id> -R buzhaodi/ios-socrates -n xnu-kernel -D xnu/out
#    -> xnu/out/kernel.development.sweet   (sha256 in xnu/out/SHA256SUMS)

# 3. the payload = kernel + ADT + boot args
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/socrates.adt \
  shim/out/payload.bin \
  --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 sweetfb_noconsole=1 sweetfb_probe=0 ufsrw=0 rootrw=0 rd=ufsroot0"

# 4. the flash image (shim + payload + the 8-block socrates vendor DTB)
shim/build-mac.sh          # the shim itself (clang + lld; byte-reproducible)
shim/build-mac-xnu.sh      # packs shim/out-mac/xnu-boot.img
#    -> verify the printed sha256 against the campaign log
```

`research/dtb/socrates-all-dtbs.dtb` (the vendor DTB table with the splash nodes injected) is a build input produced by the mik60proios campaign tooling; it is gitignored (device dump). Without it `build-mac.sh` says which file is missing.

### Patch validation before pushing

```bash
python3 tools/validate-repo.py        # series contiguity + format + replay
```

Note: the CI applies patches with `git apply` (strict, no fuzz) while validate-repo replays with GNU `patch -f` (fuzz allowed) — a patch that passes validate-repo can still be refused by the CI. When editing `xnu/patches/0028` (whose context 0030/0034 patch byte-for-byte), verify with a strict local chain of the whole series or put the change in a new post-series patch (e.g. `0187`).

## Flash and read the result

The phone has no serial port — the panel is the only console, and photos of it are the evidence channel (OCR: `~/gittest/mik60proios/tools/vision-ocr.swift`).

```bash
# 1. boot Android fully once, unlock, wait ~30 s  (clears dirty TZ state)
# 2. one temporary boot per bootloader session:
fastboot boot ~/gittest/ios-socrates/shim/out-mac/xnu-boot.img
# 3. photograph the panel (the kernel log) — including the SWEET UFS lines
```

Expected success for the M4 probe: the kernel reports `SWEET UFS: "userdata" on LUN 0: first block 0x…, N MiB, read-only`, sets `BSD root: ufsroot0`, and then panics at `mountroot` (userdata is still f2fs — that panic is the probe's success signal). After any attempt, `fastboot reboot bootloader` before the next `fastboot boot`.

## Repository layout

- `xnu/patches/` — the 187-patch series on xnu-8796.141.3 (the SWEET machine config + drivers; socrates work: 0010/0012 early console, 0028 UFS root + wake, 0187 the ABL-handoff dance)
- `shim/` — the boot shim (aarch64 freestanding, builds with Homebrew clang+lld)
- `tools/` — `mkadt.py` (Apple device tree builder), `mkpayload.py`, `mkbootimg_v0.py`, `qemu-sweet.py` (the QEMU harness), `build-ios-root.sh` (iPhone X root builder, needs Linux)
- `wifi/` — the LinuxKPI compat layer for ath11k; `userland/` — Mesa, the CoreAnimation bridge, audio, Bluetooth
- `.github/workflows/xnu.yml` — the kernel CI (macos-15 runners, Xcode 15 era iig)
- `PORTING-SOCRATES.md` — **the campaign log**: plan, device fact bank, every flash round
- `NOTES.md` — the active agent handoff block at the top

Kept out of git: partition backups, device dumps, firmware, Apple's files, logs (see `.gitignore`).

## Credits and license

- **[aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo)** — the recipe this fork carries: the shim, the XNU patch series, the tools, the userland.
- **[LakoMoor/ios-beryllium](https://github.com/LakoMoor/ios-beryllium)** — the portability proof, and `mkbootimg_v0.py`.
- **Apple's open-source releases:** xnu, IOHIDFamily, libpthread, hfs. iOS itself belongs to Apple and is not distributed here.
- Upstream's credits — hakios-apollo, HackiOS, ios-spes, ios-sailfish, [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Mesa, BearSSL, Linux ath11k, FreeBSD LinuxKPI, Xiaomi's kernel source, OrangeFox — apply in full; see upstream's README.

MIT License, see [`LICENSE`](LICENSE).
