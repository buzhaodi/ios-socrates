# ios-socrates

**Porting Apple's iOS 16 to run natively on the Xiaomi Redmi K60 Pro** (`socrates`, Snapdragon 8 Gen 2 / SM8550).

This repository is a **clone of [aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo), modified for the Redmi K60 Pro** — same goal, different phone. The upstream project runs the **unmodified iOS 16.7.16 (20H392) system of the iPhone X** — launchd, SpringBoard, the stock apps — on the Mi 10T (SM8250) with Apple's open-source XNU kernel (xnu-8796) patched for that hardware; no emulator, no Android underneath. [LakoMoor/ios-beryllium](https://github.com/LakoMoor/ios-beryllium) (Poco F1, SDM845) proved the porting method transfers between phones. This fork is the SM8550 port: keep the recipe, replace the hardware constants.

> [!WARNING]
> This is an experiment, not a daily-use phone OS. As in the upstream project, most of Apple's security model is switched off, and installing the iOS root **erases Android's `userdata` partition**.

---

## Status

The port has XNU running on the phone; the remaining milestones bring up iOS proper. Every test boot is a temporary `fastboot boot` — nothing is flashed. The full plan, the device fact bank and the campaign log live in [`PORTING-SOCRATES.md`](PORTING-SOCRATES.md).

| # | Milestone | Status | Evidence |
| --- | --- | --- | --- |
| 1 | Shim boots, draws on the panel | ✅ Done | `fastboot boot` → shim probe report on the 1440×3200 panel (gzip kernel + the 8-block socrates vendor DTB table) |
| 2 | Internal UFS read | ✅ Done | TUR rc=0, REPORT LUNS (6 LUNs), SSU rc=0 after a 19-round bring-up; the socrates UFSHCI 4.0 wake-up recipe is in `PORTING-SOCRATES.md` |
| 3 | XNU reaches fbcon | ✅ Done | **On hardware** (2026-11-06): QEMU run6 green, then the flash #2 photo shows the full SWEET driver set, `AppleCLCD 1440x1024 stride 5760 at 0xb8000000`, and a live 60 Hz vsync heartbeat (`600 vsyncs, 0 skipped, 1.8 ms late`) — the GICv3 routing, arch timer and display engine all work on the real SM8550 |
| 4 | iOS root, launchd → SpringBoard | 🚧 In progress | M4 probe image built: the shim runs the M2 UFS wake-up recipe before jumping to XNU, the kernel boots with `rd=ufsroot0` so `bsd/dev/sweet_ufs.c` reports the `userdata` GPT partition on the panel. The iOS root itself (`tools/build-ios-root.sh`) and the write into `userdata` come next |
| 5 | Touch | ❌ Not started | Goodix over SPI — the one new-vendor driver to write (upstream: Novatek) |
| 6 | Buttons, backlight, CPU freq | ❌ Not started | PM8550-family keys, WLED, EPSS domains (same EPSS driver family as upstream) |
| 7 | GPU | ❌ Not started | Adreno 740 (upstream: Adreno 650) — a740 GMU firmware + Mesa a7xx |
| 8 | Wi-Fi / Bluetooth | ❌ Not started | WCN785x-class, ath11k — same driver family as upstream's QCA6390 |
| 9 | Audio / haptics | ❌ Not started | WCD9385-class codec |

Why SM8550 is expected to be *friendlier* than the beryllium port (full table in [`PORTING-SOCRATES.md`](PORTING-SOCRATES.md)): the port moves *forward* inside the SM8250 lineage — same EPSS cpufreq, same ath11k Wi-Fi family, same UFS controller address (0x1d84000), same audio codec generation. The genuinely new work: Goodix touch, the 1440×3200 panel (~1.8× upstream's frame copy cost), Adreno 740, and the SM8550 moved GIC.

## How it boots (the recipe, unchanged from upstream)

```
Xiaomi bootloader (unlocked)
  └─ fastboot boot shim-boot.img        temporary: nothing is flashed to boot
     └─ shim                            loads XNU, an Apple Device Tree and boot options
        └─ XNU kernel + drivers         UFS storage, display, touch, GPU, Wi-Fi, Bluetooth, audio …
           └─ iOS root (HFS+)           in the userdata partition
              └─ launchd → daemons → backboardd + SpringBoard → apps
```

## What changed from apollo

- `shim/` — socrates constants (framebuffer 0xb8000000 1440×3200, GIC at 0x17100000, IMEM restart reason, Mac build via clang+lld) and the SM8550 UFS bring-up
- `tools/mkadt.py` — the socrates device tree: dram, GIC, panel, UFS, EPSS CPUs at SM8550 clocks, `xiaomi,socrates|qcom,sm8550` compatible
- `tools/mkbootimg_v0.py` — from ios-beryllium; the K60 Pro's ABL accepts a gzip kernel + the 8-block vendor DTB table
- `xnu/patches/` — the 189-patch sweet series, unchanged for now; the machine config (0004) and the driver patches (touch/GPU/cpufreq/Wi-Fi) get their socrates pass as each milestone opens
- `PORTING-SOCRATES.md` — this port's fact bank, plan and campaign log (upstream's equivalent is `NOTES.md`)

Upstream's documentation — the full feature list, boot options, firmware table, `userland/` programs — describes the Mi 10T state and still applies to everything not yet socratized: see the [apollo website](https://atechgeekdev.github.io/ios-apollo/) and [`NOTES.md`](NOTES.md).

## Repository layout

Same tree as upstream: `xnu/patches/` (the kernel patches), `shim/` (the boot shim), `wifi/` (the Linux compat layer for ath11k), `userland/` (Mesa, CoreAnimation bridge, audio, Bluetooth), `tools/` (device tree builder, boot packer, iOS root builder), `.github/workflows/` (CI on macOS runners).

Kept out of git (upstream policy, unchanged): partition backups, device dumps, firmware, Apple's files, serial numbers and logs.

## Credits and license

This fork stands on the upstream projects — everything not listed as socrates work above is theirs:

- **[aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo)** — the whole recipe: the shim, the 189-patch XNU series, the tools, the userland. iOS 16 running on the Mi 10T.
- **[LakoMoor/ios-beryllium](https://github.com/LakoMoor/ios-beryllium)** — the proof that the recipe ports (SDM845), and `mkbootimg_v0.py`.
- **Apple's open-source releases:** xnu, IOHIDFamily, libpthread, hfs. iOS itself belongs to Apple and is not distributed here.
- Upstream's credits — [hakios-apollo](https://github.com/enesuzun2002/hakios-apollo), [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel), ios-spes, ios-sailfish, [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Mesa, BearSSL, Linux ath11k, FreeBSD LinuxKPI, Xiaomi's kernel source, OrangeFox — apply in full; see upstream's README.

MIT License, see [`LICENSE`](LICENSE).
