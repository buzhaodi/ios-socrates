# Port to Redmi K60 Pro (socrates, SM8550) — plan

Source: aTechGeekDev/ios-apollo (Mi 10T/Mi 10T Pro, SM8250 — the mature original; project-BadApple),
with the beryllium precedent (LakoMoor/ios-beryllium, SDM845) as the live example of the porting
method. Target: **iOS 16.7.16 (20H392, the iPhone X system) running natively on the K60 Pro** —
shim → XNU (xnu-8796 + the sweet patch series) → the unmodified iPhone X system in `userdata` →
launchd → SpringBoard.

Rules (from AGENTS.md, unchanged): nothing is flashed except the iOS root (and only with the
user's explicit confirmation at that point); every test boot is a temporary `fastboot boot`;
partition backups come first; device dumps and Apple artifacts stay out of git.

## Why socrates is expected to be *easier* than beryllium's port

We port *forward* inside the modern SM8250 lineage, while beryllium went *backward* to SDM845:

| subsystem | apollo (SM8250) | beryllium (SDM845) had to | socrates (SM8550) |
| --- | --- | --- | --- |
| CPU freq | EPSS | rewrite (OSM/CPUFREQ-HW) | **EPSS confirmed (3 domains @ 0x17d91000/20/30)** |
| Wi-Fi | QCA6390, ath11k | new layer (WCN3990, ath10k) | **same ath11k family** (WCN785x-class) |
| audio codec | WCD938x + CS35L41 | swap to WCD9341 + TFA9894 | WCD9385-class (same generation) |
| UFS HC | 0x1d84000 | same | **same address 0x1d84000** |
| GIC | GICv3 | GICv3 | GICv3 (moved to 0x17100000) |
| touch | Novatek SPI | Novatek (same vendor) | **Goodix SPI — the one new-vendor driver to write** |
| panel | 1080x2400 | 1080x2246 | **1440x3200 — 1.8x the copy cost; the fps/heat budget is ours to lose** |

## Milestones

| # | Goal | Needs | Done when |
|---|------|-------|-----------|
| 0 | Backups + device facts | Recovery with adb (OrangeFox/TWRP for socrates), live DTB | persist/modemst1/2/fsg/fsc/boot/dtbo/vbmeta (+ the chosen root partition) saved and SHA256-verified on both sides |
| 1 | Shim boots, screen colour | socrates constants in the shim (below); boot image accepted by the K60 Pro's ABL | solid colour / text on the panel from `fastboot boot` |
| 2 | UFS read | the shim's UFS driver against SM8550's controller (same HC address as apollo) | shim reads GPT + the payload/root from UFS |
| 3 | XNU reaches fbcon | mkadt.py socrates facts; kernel patch 0004 (machine config) + QEMU first (`tools/qemu-sweet.py` exists) | kernel log on the panel, no panic |
| 4 | iOS root, launchd | build-ios-root.sh (iPhone X IPSW 16.7.16), display geometry for 1440x3200 | SpringBoard drawing |
| 5 | Touch | Goodix SPI driver (new vendor — the one real new-driver job) | taps work |
| 6 | Buttons, backlight, CPU freq | PM8550-family GPIO keys + WLED; EPSS domains | buttons/backlight/clocks work |
| 7 | GPU | Adreno 740: a740 GMU firmware, zap shader; Mesa freedreno a7xx | GPU-drawn UI |
| 8 | Wi-Fi / BT | WCN785x: ath11k (same driver family as apollo's QCA6390) | scan, WPA2, internet |
| 9 | Audio / haptics | WCD9385-class codec + amps; haptic motor | sound, vibration |

## Device fact bank (mined from the socrates vendor DTB — `dtb_00..07`, Kalama QCM block — plus the mik60proios UniXNU campaign, all hardware-proven on this phone)

SoC facts (from the Kalama QCM DTB block):
- **GICv3**: GICD `0x17100000`/0x10000, GICR `0x17180000`/0x200000 (1 region, stride 0x40000),
  ITS `0x17140000`, maintenance IRQ 9. (apollo: 0x17a00000/0x17a60000 — moved on SM8550.)
- **Arch timer**: 19.2 MHz (`0x124f800`); vtimer PPI 11; memtimer frame `0x17420000`.
- **CPUs**: 8x Kryo, **MPIDR 0x000-0x700**, PSCI enable-method, clusters
  silver 0-3 / gold 4-6 / prime 7 (capacity-dmips differ per node).
- **cpufreq**: `qcom,cpufreq-epss`, 3 domains at `0x17d91000`, `0x17d92000`, `0x17d93000`
  (same EPSS driver family as apollo's sweet_cpufreq).
- **UFS**: HC `0x1d84000`/0x3000 (same address as apollo), ICE `0x1d88000`/0x8000,
  ICE_HWKM `0x1d90000`/0x9000, IRQ 265, 2 lanes/direction, UFS 4.x device.
- **RPMh apps_rsc**: `0x17a00000` (drv-0/1/2 = 0x17a00000/0x17a10000/0x17a20000, TCS inside).
- **PSCI**: cpu-release-addr `0xe3940000` (cold-boot path; use PSCI calls).

Board facts (from the mik60proios campaign — BRINGUP-LOG §1-8, all proven on hardware):
- **cont_splash / framebuffer**: `0xb8000000` + `0x2b00000` (the stock uefiplat "Display
  Reserved" window; same values sheng/vermeer carry). **Panel 1440x3200@60**, BGRA, stride
  `1440*4`. The kernel-side frame copy budget is ~18 MB/frame vs apollo's ~10 — plan for the
  GPU page-flip path earlier than apollo needed it.
- **The ABL's SoC/board match**: hard on `chip_id & 0xffff` against the concatenated vendor
  DTB table — a single Kalama QCM block never leaves the bootloader. The boot image must
  carry the whole 8-block table (msm-id 0x25c/0x258/0x207/0x25b/0x259/0x218); splash-region
  nodes are injected into EVERY block (`mik60proios/tools/inject-nodes-mac.sh` is the working
  reference for both quirks).
- **fastboot boot**: one `fastboot boot` per bootloader session; after any attempt,
  `fastboot reboot bootloader` first. (beryllium hit the same quirk on SDM845.)
- **Dirty-state pollution**: after any kernel crash/reset, subsequent `fastboot boot`s reset
  silently at the entry (loader page or no kernel band). The cure, proven 6+ times: boot
  fully into Android once, unlock, wait ~30 s, then flash. **No SMCs from the loader** on
  this board (a TZ secure-watchdog SMC at the handoff killed every boot on dirty TZ state;
  kernel-runtime SMCs like RPMh are fine — apollo does them).
- **Conventional memory**: below 4 GiB in ≤16 MiB pieces, at most 1 GiB conventional below
  4 GiB usable for the boot payload; the fixed-region lesson (the "DRAM gap 0xc7cc0000")
  applies to any pre-handoff window scan. XPU-protected holes exist in the map — the shim's
  memory map must respect them (mik60proios UEFI campaign, BRINGUP-LOG §3-4).
- **Serial**: none wired out of the box (no engineering cable) — **the panel is the only
  channel**; screen-photo OCR tooling transfers from mik60proios (`tools/vision-ocr.swift`).

Donor identity (from the iPhone X IPSW, d22ap):
- research/ipsw-x/d22ap.adt: the /product identity, haptics node, display canvas — extract with
  `tools/remotezip.py` + `tools/adt.py` once the IPSW (16.7.16 20H392, iPhone10,3/iPhone10,6)
  is downloaded. iOS root build: `tools/build-ios-root.sh` (needs Linux: apfs-fuse, hfsprogs —
  or WSL) + ~40 GB of scratch space.

## Apollo-specific code to replace (grep order — adapt as beryllium did)

- `shim/shim.c`, `shim/ufs.c` — memory map, UFS controller (HC address shared; ICE/HWKM new)
- `tools/mkadt.py` — the constants block at the top (PHYS_BASE/MEM/HIGH_*, GICD/GICR, FB,
  FB_W/H/STRIDE, UFS, TIMEBASE), /cpus, /arm-io reg maps
- `xnu/patches/0004` machine config (+ the SOC_BASE assumptions inside the series:
  0125 `sweetfb_probe` reads MDP registers — gate it `sweetfb_probe=0` like beryllium's boot 1)
- `0083-0094` touch (Novatek -> **Goodix**), `0095/0098/0153` cpufreq+thermal (EPSS — mostly
  retuning), `0100-0112` GPU/GMU/ZAP (a650 -> a740), `0126-0135` Wi-Fi (ath11k — retune),
  `0136-0145` BT, `0139` audio, `0147` haptics, `0148/0153` DDR (RPMh)
- `tools/pull-*-fw.sh`, `nvt-fw-find.py` (touch firmware — Goodix equivalent),
  `gmu-fw-info.py` (a740 GMU), `acdb-mic.py`
- `userland/mesa/sweet-drm` (a7xx), `userland/audio`, `userland/bt-inject`

## Boot image (M1 detail)

The K60 Pro's ABL expects an Android boot image whose dtb table covers its SoC id. beryllium
needed `Image.gz + appended DTB`; the K60 Pro ABL has accepted a raw image kernel in the
UniXNU campaign (BootShim). Plan: raw shim image first, gzip+dtb as fallback; the 8-block
vendor dtb table + per-block splash injection from the mik60proios tooling either way.

## Campaign log

### 2026-11-06 — repo forked from apollo; facts banked
- `ios-socrates` created from aTechGeekDev/ios-apollo (109 MB tree, 189 kernel patches,
  full tools/shim/userland stack) on branch `socrates`; `ios-beryllium-ref` cloned beside
  it as the live porting example.
- Device facts mined from the socrates vendor DTB table (above) — everything needed for
  M1-M3 constants is in hand; the panel/touch/audio nodes for M5+ live in the board-specific
  DTB blocks (mined when those milestones open).
- iPhone X IPSW (16.7.16, 20H392) URL handed to the user for manual download (proxy-free
  policy for multi-GB artifacts): research/ipsw/ is the destination, gitignored.
- mik60proios (the UniXNU campaign repo) stays as the socrates hardware-fact archive and
  tool source; nothing there is modified by this campaign.

### 2026-11-06 — IPSW landed; socrates parameterization done (M1/M3 constants)
- IPSW (iPhone10,3/iPhone10,6 16.7.16 20H392, 5.75 GB) downloaded by the user into
  research/ipsw/; `DeviceTree.d22ap.im4p` extracted and unwrapped with `tools/adt.py`
  (pure python + lzfse, no blacktop ipsw CLI needed) -> research/ipsw-x/d22ap.adt
  (186 KB, target-type D22 — the identity donor is sound).
- socrates constants landed in three files:
  - shim/console.h: FB_BASE 0xb8000000, 1440x3200 (stride auto).
  - shim/shim.c: IMEM_RESTART_REASON 0x146aa65c (imem@146aa000 restart_reason@65c — the
    offset survives, the base moved), GICD 0x17100000 / GICR 0x17180000 / size 0x200000 /
    **PE stride 0x40000** (SM8250 was 0x20000 — the socrates DTB says 0x40000), PSHOLD kept
    0x0c264000 (stable across SoCs, socrates-unverified), VTIMER 27 unchanged.
  - tools/mkadt.py: dram 0x880000000+256 MiB (the campaign-proven clean window — see the
    comment; grow from M1's ABL memory dump, never by guessing), GIC as above (ADT reg
    0x16100000/0x16180000 verified in the dump), FB 0xb8000000+0x2b00000, panel 1440x3200,
    UFS 0x1d84000 (same address as apollo), TIMEBASE 19200000 unchanged, board compatible
    xiaomi,socrates|qcom,sm8550, CPUs qcom,kryo with SM8550-AB clocks (2.02/2.8/3.19 GHz).
- `tools/mkadt.py shim/out/socrates.adt --dump` builds a verified 12.9 KB ADT: dram-base
  0x880000000, iPhone10,3/D22 identity from the donor, gic regs correct.
- NOT done: the shim itself is not yet compiled (needs aarch64-linux-gnu-gcc: WSL apt or a
  Mac clang/ld.lld adaptation of shim/build.sh) and the kernel build path is undecided
  (GitHub Actions like apollo — needs this repo pushed — vs local Xcode 15.4).

### 2026-11-06 — the shim builds on the Mac; the M1 image is ready to flash
- shim/build-mac.sh: apollo's build.sh ported to homebrew clang + lld (WSL gcc not needed).
  Two clang-vs-gcc differences fixed in source, both documented in the script:
  clang's switch/lookup-table transform materialized the exception-name strings as an
  R_AARCH64_ABS64 .rodata pointer table (the shim forbids absolute relocations - the ABL
  runs the image unrelocated) -> char[4][8] array instead; and __builtin_strcmp became a
  libc call -> a freestanding strcmp in shim.c. _end parses via python (macOS awk has no
  strtonum).
- The M1 flash image: shim Image (184,320 B) + the 8-block splash-injected socrates vendor
  DTB blob (mik60proios tools' output, copied to research/dtb/, gitignored) + a
  deterministic empty newc ramdisk, Android boot header v0 (the header the K60 Pro's ABL
  has accepted all campaign long). tools/mkbootimg_v0.py borrowed from ios-beryllium.
  build-mac.sh is byte-reproducible: shim-boot.img sha256 0b3687061765068c.
- One flash covers M1 AND M2: the shim paints its probe report on the panel (CPU feature
  registers, PSCI/SMCCC, 8-core PSCI CPU_ON, GICv3 vtimer IRQ, the UFS probe + partition
  list) - and the socrates memory constants get their first hardware proof. A fault
  paints a blue panic screen with the exception context.

### Next concrete actions
1. (user) Flash shim/out-mac/shim-boot.img (full Android boot first, one fastboot boot
   per bootloader session) and photograph the panel - the M1/M2 evidence.
2. (decision) Kernel build: create the GitHub repo + push + run apollo's xnu.yml CI
   (free on public repos), or install Xcode 15.4 locally.
3. (user+agent) M0 backups over a recovery image (OrangeFox/TWRP for socrates) — needs the
   phone at hand; nothing destructive.
