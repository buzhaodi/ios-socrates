# ios-socrates

A native **iOS 16.7.16 (20H392) port for the Redmi K60 Pro** (`socrates`, Snapdragon 8 Gen 2 / SM8550). The goal is to boot the iPhone X system on a patched XNU kernel, without Android underneath. **Current hardware progress: XNU boots, reads the internal UFS/GPT and registers `userdata` as its root device. The iOS root is installed in userdata: hardware photos confirm HFS mounting and PID 1 launchd executing early boot tasks. SpringBoard and a working iOS interface are not yet verified.**

This repository targets **K60 Pro only**. It inherits the [ios-apollo](https://github.com/aTechGeekDev/ios-apollo) foundation and uses [ios-beryllium](https://github.com/LakoMoor/ios-beryllium) and [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) as porting references. A donor implementation is not evidence that its driver works on this phone.

**Active branch:** `socrates` in [buzhaodi/ios-socrates](https://github.com/buzhaodi/ios-socrates).

> [!WARNING]
> This is an experiment, not a daily-use phone OS. Most of Apple's security model is switched off, and installing the iOS root **erases Android's `userdata` partition**. Kernel test images use temporary `fastboot boot`. The iOS root was written only to `userdata` after explicit user confirmation; bootloaders, recovery, modem, persist and GPT were not flashed.

---

## Status — 2026-11-08

**Flash #17 passed the kernel-side storage probe on hardware.** The screen shows:

```text
SWEET UFS: "userdata" on LUN 0: first block 0x2f7840, 229451 MiB, read-only
SWEET UFS: socrates root ready; unported SM8250 peripherals skipped
BSD root: ufsroot0B, major 1, minor 0
```

HFS then rejects the existing partition content (`unknown Volume Signature: 152e`, mount error 22 / root errno 19) and retries the mount. This is the expected endpoint of the current read-only probe: at the time of flash #17, Android data was still in `userdata`. Since then the user approved installation and the verified iOS HFS+ root was written there. Flash #16's data abort in the inherited SM8250 CPU-frequency driver is avoided by patch 0189, which skips unported peripheral initialization after registering the K60 Pro root device. **CPU frequency control is not yet adapted or enabled.**

| # | Milestone | Status | Hardware evidence / remaining work |
| --- | --- | --- | --- |
| 0 | Critical-partition backups | Done locally (limited scope) | 126 partitions + 12 GPT ranges, 138 verified files (~3.153 GiB); device/local hashes and GPT CRCs pass. Personal userdata and stock super/cust images omitted; backups never uploaded |
| 1 | Shim and panel console | Done | Temporary boot displays the shim report on the 1440×3200 panel |
| 2 | Shim UFS read | Done | TUR, six LUNs and GPT enumeration verified during the M2 campaign |
| 3 | XNU bootstrap and console | Done | Kernel runs on SM8550 with GIC/timer interrupts and a live 60 Hz framebuffer heartbeat; console currently uses 1440×1024 |
| 4a | Kernel UFS / GPT / BSD root device | Done | Flash #17 reads `userdata` on LUN 0, registers `ufsroot0B`, and reaches filesystem mount attempts |
| 4b | Build and install iOS root | Installed; HFS hardware pass | Verified 16 GiB root; 24 sparse fragments written successfully; boot #18 recognizes iOSRoot and reaches real launchd. Full device readback hash not performed |
| 4c | launchd → backboardd → SpringBoard | launchd running; UI pending | Boot #18 shows DONE EARLYBOOT SETUP / DONE UMD_INIT and real launchd boot jobs; backboardd/SpringBoard rendering not yet verified |
| 5 | Goodix touch | Hardware identified; port pending | Live Android reports `goodix_ts`; sheng uses Novatek, so its controller protocol cannot be copied directly |
| 6 | Buttons, backlight, CPU frequency | Port pending | Live CPU topology is **3+4+1** (CPUs 0–2 / 3–6 / 7); sheng EPSS/L3 implementation reviewed; old SM8250 drivers remain skipped |
| 7 | GPU acceleration | Hardware identified; port pending | Live GPU is **Adreno 740v2**; sheng A740, GMU/SMMU and Metal/Turnip components reviewed; no accelerated rendering verified on K60 Pro |
| 8 | Wi-Fi / Bluetooth | Port pending | sheng ath12k is a candidate; exact K60 Wi-Fi chip/PCI ID and board firmware still need verification |
| 9 | Audio / haptics | Port pending | K60 board-specific codec, amplifier and motor integration remain |

Latest verified build: [GitHub Actions 42396141835](https://github.com/buzhaodi/ios-socrates/actions/runs/42396141835), kernel source commit `08b23ef`, patches **0001–0189**. Local validation passes, including 20 executable UFS/board-selection regression cases.

- Verified read-only storage probe #17: `shim/out-mac/xnu-boot.img`
- Image SHA-256: `8ba7285acb631e178468b7db3d57723f4850455c70d6e0f3792243f3d5723f0a`
- Kernel UUID: `13B38CEA-2E65-3B80-8BA1-E1DA7873C5D5`

Full device facts, source pins and flash-by-flash results: [`PORTING-SOCRATES.md`](PORTING-SOCRATES.md). Active handoff: [`NOTES.md`](NOTES.md).

## Next steps

1. Let root boot #18 finish its initial launchd tasks and capture backboardd/SpringBoard progress or a stable error/panic. HFS and launchd now have screen evidence; a working interface is still pending.
2. Keep the verified local backups and root images intact. `userdata` now contains the installed iOS root; personal Android data was not retained. The previous read-only probe image remains available, but current root-boot tests use `shim/out-mac/xnu-ios-root-boot.img` (SHA-256 `24f5852b6d42014ff19e39b692ec14da3a791d2c9433838691ea333ba178beb5`).
3. Bring up display/userland and adapt K60 Pro peripherals individually. Start CPU work with a read-only EPSS/L3 probe; use the banked sheng references for GPU, SPI/HID and Wi-Fi integration. Correct remaining CPU clock metadata in `mkadt.py` before enabling SMP/governor work.

## How it boots

```
Xiaomi bootloader (unlocked)
  └─ fastboot boot xnu-ios-root-boot.img   temporary kernel boot
     └─ shim                        loads XNU + the Apple device tree, wakes the UFS
        └─ XNU kernel (SWEET patches)  189-patch series on xnu-8796.141.3
           └─ ufsroot0B / userdata      storage probe verified; HFS+ root now installed
              └─ iOS root (HFS+)        installed after user confirmation
                 └─ launchd → backboardd → SpringBoard   launchd confirmed; UI pending
```

## Build a read-only kernel probe (macOS)

Mac with Xcode command-line tools, Homebrew `clang/llvm`, and `gh` logged in. Python 3 needed. Kernel CI runs on GitHub Actions (macOS runners) — a local kernel build is not required; the kernel binary is downloaded from the CI artifact.

```bash
# 0. one-time setup
brew install llvm gh
gh auth login

# 1. the device tree (Apple-format ADT; socrates constants inside)
python3 tools/mkadt.py shim/out/socrates.adt

# 2. the kernel — dispatch CI for the active branch:
git push origin socrates
gh workflow run xnu.yml --ref socrates -R buzhaodi/ios-socrates
gh run list -R buzhaodi/ios-socrates -w xnu.yml -b socrates
# Replace YOUR_RUN_ID with the numeric ID from the list above.
RUN_ID=YOUR_RUN_ID
gh run watch "$RUN_ID" -R buzhaodi/ios-socrates --exit-status
gh run download "$RUN_ID" -R buzhaodi/ios-socrates -n xnu-kernel -D xnu/out
#    -> xnu/out/kernel.development.sweet   (sha256 in xnu/out/SHA256SUMS)

# 3. the payload = kernel + ADT + boot args
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/socrates.adt \
  shim/out/payload.bin \
  --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 sweetfb_noconsole=1 sweetfb_probe=0 ufsrw=0 rootrw=0 rd=ufsroot0"

# 4. the temporary boot image (shim + payload + the 8-block socrates vendor DTB)
shim/build-mac.sh          # the shim itself (clang + lld; byte-reproducible)
shim/build-mac-xnu.sh      # packs shim/out-mac/xnu-boot.img
#    -> verify the printed sha256 against the campaign log
```

`research/dtb/socrates-all-dtbs.dtb` (the vendor DTB table with the splash nodes injected) is a build input produced by the mik60proios campaign tooling; it is gitignored (device dump). Without it `build-mac.sh` says which file is missing.

### Patch validation before pushing

```bash
python3 tools/validate-repo.py        # series contiguity + format + replay
python3 tests/test_socrates_ufs.py    # UFS protocol + board-selection regression cases
```

Note: the CI applies patches with `git apply` (strict, no fuzz) while validate-repo replays with GNU `patch -f` (fuzz allowed) — a patch that passes validate-repo can still be refused by the CI. When editing `xnu/patches/0028` (whose context 0030/0034 patch byte-for-byte), verify with a strict local chain of the whole series or put the change in a new post-series patch (the next unused patch number).

## Temporary boot and read the result

The phone has no serial port — the panel is the only console, and photos of it are the evidence channel (OCR: `~/gittest/mik60proios/tools/vision-ocr.swift`).

```bash
# Before the first root installation, Android was used to establish clean TZ state.
# With the iOS root installed, enter FASTBOOT directly (volume down + power).
# One temporary boot per bootloader session:
fastboot boot ~/gittest/ios-socrates/shim/out-mac/xnu-ios-root-boot.img
# 3. photograph the panel (the kernel log) — including the SWEET UFS lines
```

The M4 storage-only probe is now verified (flash #17): the kernel reports `SWEET UFS: "userdata" on LUN 0: first block 0x…, N MiB, read-only`, sets `BSD root: ufsroot0`, and reached HFS mount failures/retries before root installation. This proves storage access, not an iOS boot. After a large userdata flash, a software bootloader reboot may leave Mac USB transfers stuck at Sending. A physical volume-down + power reset to FASTBOOT restored boot transfer for #18. Keep kernel images temporary; ordinary Android boot now requires restoring its data filesystem.

## Repository layout

- `xnu/patches/` — the 189-patch series on xnu-8796.141.3 (the SWEET machine config + drivers; socrates work: 0010/0012 early console, 0028 UFS root + wake, 0187 root diagnostics, 0188 standard UFS protocol fixes, 0189 the socrates peripheral gate)
- `shim/` — the boot shim (aarch64 freestanding, builds with Homebrew clang+lld)
- `tools/` — `mkadt.py` (Apple device tree builder), `mkpayload.py`, `mkbootimg_v0.py`, `qemu-sweet.py` (the QEMU harness), `build-ios-root.sh` (iPhone X root builder, needs Linux)
- `wifi/` — the inherited LinuxKPI compat layer for ath11k (not yet a verified K60 Wi-Fi driver); `userland/` — Mesa, the CoreAnimation bridge, audio, Bluetooth
- `.github/workflows/xnu.yml` — the kernel CI (macos-15 runners, Xcode 16-compatible iig support)
- `PORTING-SOCRATES.md` — **the campaign log**: plan, device fact bank, every flash round
- `NOTES.md` — the active agent handoff block at the top

Kept out of git: partition backups, device dumps, firmware, Apple's files, logs (see `.gitignore`).

## Credits and license

- **[aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo)** — the recipe this fork carries: the shim, the XNU patch series, the tools, the userland.
- **[LakoMoor/ios-beryllium](https://github.com/LakoMoor/ios-beryllium)** — the portability proof, and `mkbootimg_v0.py`.
- **[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) and its shared driver components** — SM8550 CPU/GPU/input/Wi-Fi references; migration findings and pinned revisions are recorded in the porting log.
- **Apple's open-source releases:** xnu, IOHIDFamily, libpthread, hfs. iOS itself belongs to Apple and is not distributed here.
- Upstream's credits — hakios-apollo, HackiOS, ios-spes, ios-sailfish, [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Mesa, BearSSL, Linux ath11k, FreeBSD LinuxKPI, Xiaomi's kernel source, OrangeFox — apply in full; see upstream's README.

MIT License, see [`LICENSE`](LICENSE).
