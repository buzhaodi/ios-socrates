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
| CPU freq | EPSS | rewrite (OSM/CPUFREQ-HW) | **EPSS confirmed (3 domains @ 0x17d91000/20/30; 3+4+1 cores)** |
| Wi-Fi | QCA6390, ath11k | new layer (WCN3990, ath10k) | **ath12k donor available; exact K60 radio ID pending** |
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
| 8 | Wi-Fi / BT | identify K60 radio; sheng ath12k candidate for WCN7850 | scan, WPA2, internet |
| 9 | Audio / haptics | WCD9385-class codec + amps; haptic motor | sound, vibration |

## Device fact bank (mined from the socrates vendor DTB — `dtb_00..07`, Kalama QCM block — plus the mik60proios UniXNU campaign, all hardware-proven on this phone)

SoC facts (from the Kalama QCM DTB block):
- **GICv3**: GICD `0x17100000`/0x10000, GICR `0x17180000`/0x200000 (1 region, stride 0x40000),
  ITS `0x17140000`, maintenance IRQ 9. (apollo: 0x17a00000/0x17a60000 — moved on SM8550.)
- **Arch timer**: 19.2 MHz (`0x124f800`); vtimer PPI 11; memtimer frame `0x17420000`.
- **CPUs**: 8x Kryo, **MPIDR 0x000-0x700**, PSCI enable-method, clusters
  silver 0-2 / gold 3-6 / prime 7 (**3+4+1**, corrected by live Android
  policy0/policy3/policy7 on2026-11-08; earlier4+3+1 summary was wrong).
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

### 2026-11-06 — M2 DONE: TUR rc=0, REPORT LUNS: 6 LUNs, SSU rc=0 — 19 flash rounds to a green SCSI bus
- The full HCE reset + DME_POWERON + DME_LINK_STARTUP + FORCED bDeviceInit
  sequence was the key. 19 flash rounds from the first M1 image to the first
  green SCSI bus, each round decoding one layer:
  - v1-v4: sense channel opened (v3 sense offsets corrected; v4 REPORT LUNS probe)
  - v5-v7: fDeviceInit query channel built (v5 wrong type, v6 real types + W-LUN,
    v7 SSU + W-LUN queries — all silently dropped because...)
  - v8-v11: canonical UPIU header + UTRD (v8 real header, v9 command_type=1,
    v10 OCS instrumentation, v11 OCS completion poll)
  - v12: fDeviceInit block moved AFTER the UTRL takeover (the queries were
    ringing the doorbell on the ABL's old list — the most humbling bug)
  - v13-v14: response code at [6] + READ_FLAG first (v13) + REPORT LUNS (v14)
  - v15-v16: READ_ATTR — bBootLUNEn=0, bPowerMode=0 (v15 wrong offset, v16 right)
  - v17: WRITE_ATTR bBootLUNEn rejected (GENERIC_FAILURE) + SSU both modes rejected
  - v18: HCE reset + DME_POWERON + DME_LINK_STARTUP — the device SAW it
    (UNIT ATTENTION 06/29 on the first SSU after the reset)
  - v19: forced SET bDeviceInit even when the flag reads 0 (the HCE reset put
    the device back into needs-init state; the flag just hadn't been re-armed)
    + fixed UIC polling (wait bit drop then rise, not just rise) = **TUR GREEN**
- The socrates UFS wake-up recipe (for PORTING-SOCRATES.md and the kernel's
  own UFS driver):
  1. Stop the transfer list (UTRLRSR=0)
  2. HCE reset (HCE=0, 100ms, HCE=1, wait HCS.DP)
  3. UIC DME_POWERON (cmd 0x10 to 0x90, poll 0xA0 bit0 drop→rise)
  4. UIC DME_LINK_STARTUP (cmd 0x16 to 0x90, poll same)
  5. Re-take the transfer list (UTRLRSR=0, UTRLBA/UTRLBAU, UTRLRSR=1)
  6. SET_FLAG bDeviceInit (query 0x16, opcode 0x06, idn 0x01, cs=0x02, qfunc 0x81)
  7. Poll READ_FLAG bDeviceInit until it clears (query 0x16, opcode 0x05, qfunc 0x01)
  8. NOP → TUR → CAP → READ → GPT
- Panel evidence: `DEVINI set rc=0`, `poll rc=0 flag=0`, `NOP rc=0 resp0=0x20`,
  `RLUNS rc=0 luns=6 first=0`, `SSU rc=0`, `TUR rc=0 ocs/st=0`

### 2026-11-06 — M3 QEMU campaign: the kernel is GREEN in QEMU; three landmines defused (all payload-side)
- Kernel: CI run 42197633756 (xnu-kernel artifact, sha d2e5ab94… supersededed by 3a137b2b…
  after 0010/0012 were socratized - FB 0xb8000000, 1440x3200/5760; validate-repo green).
  The ONLY kernel-side socrates work so far: patches 0010/0012 (early console). Everything
  else in the 189-patch series runs as-is.
- QEMU rounds (tools/qemu-sweet.py, virt board, 35GB RAM to cover dram 0x880000000):
  - run1/2 PANIC pmap_map_bd "no PTE for vaddr 0xfffffe000c000000": the early io_map VA
    window (arm_vm_init: 2MB + 10MB per 256MB of kernel RAM + the boot Video bytes) is too
    small for the 45MB fbcon map - apollo only fit because it runs 1232MB of kernel RAM
    (window 72MB). Discriminator proof: apollo's own FB constants panic at the IDENTICAL
    VA in our harness.
  - run3 PANIC same site with the ADT fbcon shrunk to the visible 18MB: backtrace
    recovered from the intstack dump (qemu-sweet now saves pram.raw + kstatics.raw):
    arm_init -> PE_create_console -> initialize_screen -> io_map. THREE maps share the
    window: fbcon (ADT) + boot screen + text console (both Video.v_height*v_rowBytes).
    Budget: 18MB + 2*V <= 22MB + V  =>  v_height=1024 (5.6MB) fits all three.
  - run4 PANIC "Unable to find driver for this platform: D22AP": the M2 socrates pass had
    replaced 'xiaomi,sweet' in the ADT root compatible - SweetPlatformExpert (xnu 0016)
    matches ONLY that string. Restored; socrates/sm8550 ride along for identification.
  - run5 abort in sweet_fb_probe (xnu 0125): it maps SM8250's MDP 0x0ae01000 - SM8550's
    display engine lives elsewhere. cmdline sweetfb_probe=0 (the PORTING plan's own
    pre-judgment, now proven).
  - run6 GREEN, 60 s no panic: platform expert registered, the full SWEET driver set
    (AKS/WDT/SURF/HID/battery/backlight), AppleCLCD "1440x1024 stride 5760 at 0xb8000000"
    with a live 60Hz vsync heartbeat, VM bootstrap complete.
- Fixes, all payload-side (no kernel rebuild): boot_args Video.v_height=1024 as the console
  VA budget (loader.c + qemu-sweet.py), ADT root compatible keeps xiaomi,sweet, ADT
  sweet-fbcon reg = the visible 18MB (not the 45MB window), cmdline sweetfb_probe=0.
- The M3 flash image: shim/build-mac-xnu.sh -> out-mac/xnu-boot.img (21MB, sha 35d415a4…):
  shim Image.gz + the SWEETXNU payload as the ramdisk (apollo's recipe) + the 8-block
  socrates DTB blob.

### 2026-11-06 — first M3 flash: the SAME pmap_map_bd panic — the packed shim was STALE; rebuilt + gated
- Panel photo (OCR): `initialize_screen: b=0xb8000000, w=0x5a0, h=0xc80, r=0x1680`, then
  `panic(cpu 0, caller 0xfffffe0007baf060): pmap_map_bd @pmap.c:1885` + nested-panic stall —
  the IDENTICAL site/VA as QEMU runs 1-3. First suspicion: the fix didn't work on hardware.
  It didn't have to: the flashed image was a hybrid.
- Forensics (all observations, not guesses):
  - The flashed `xnu-boot.img` embedded the SAME `Image.gz` as `out-mac/` (byte-identical,
    sha 03e2142c…), whose loader_boot disassembly stores `mov w10,#0xc80; str [x19,#0x48]`
    — v_height=3200, the PRE-fix loader. Image.gz mtime 17:08 predates the loader.c fix 18:33.
  - The payload half was NEW: photo `h=0xc80` = 18MB/5760 = 3200 rows proves the ADT fbcon
    reg was the fixed visible window (QEMU run2's old 45MB ADT printed h=0x2000=8192 rows);
    cmdline `sweetfb_probe=0` verified in the packed payload.
  - So: 18MB fbcon (new) + v_height=3200 boot_args (old) = 18+3×15.6… still over the 32MB
    early window -> the exact panic the fix was meant to kill. QEMU never caught it because
    qemu-sweet.py builds boot_args in python and never executes the shim loader at all.
- Fix: build-mac.sh re-run (shim now stores 0x400=1024, verified by disassembly),
  build-mac-xnu.sh re-run. xnu-boot.img sha cfc40eb2… (was 35d415a4…). Verified INSIDE the
  final image: embedded Image.gz == fresh build; `mov w10,#0x400; str x10,[x19,#0x48]`
  present; the 0xc80 encoding absent; payload ADT fbcon reg = 0x1194000; cmdline gate intact.
- Process fix: build-mac-xnu.sh now refuses to pack when out-mac/Image.gz is older than any
  shim source, or the payload older than mkadt.py/mkpayload.py (tested both directions).

### 2026-11-06 — M3 DONE ON HARDWARE: the re-flash is GREEN (photo-decoded)

- Flash #2 (image cfc40eb2…, the rebuilt one): **no panic anywhere on the panel.** OCR of
  the photo, top to bottom: Darwin banner (xnu-8796.141.3), VM bootstrap done (85 maps,
  165 entries, 501 holes left), RTM/scheduler/telemetry, SWEET PRNG + pthread + AMFI
  registered, SWEET platform expert registered + device-tree expert started, the full
  SWEET driver set (AKS/WDT/SURF/HID buttons+touch/BATTERY/BL), then the M3 finish line:
  - `SWEET FB: AppleCLCD 1440x1024 stride 5760 at 0xb8000000` — the console-budget fix
    live on hardware (1024 = the io_map budget; the real 3200 geometry comes from the ADT).
  - `SWEET FB: 19.8 s: iOS gave 9 frames, 9 reached the panel; 600 vsyncs, 0 skipped,
    latest 1.8 ms late` — the 60 Hz vsync interrupt is LIVE on hardware: GICv3 routing,
    arch timer and the display engine all work on the real SM8550. More than QEMU proved.
- The kernel idles at the vsync heartbeat by design: the UFS root probe only runs with
  boot-arg rd=ufsroot0, which this M3 payload's cmdline does not carry. Nothing was
  exercised past bootstrap - that is M4.
- M4 facts pinned down while reading xnu 0028 (bsd/dev/sweet_ufs.c):
  - The kernel driver REUSES the bootloader's live UFS link (HCE/DP/UTRLRDY checked,
    transfer list re-taken, TUR with 3 unit-attention retries) — it does NOT bring the
    device up itself. On socrates the ABL leaves the link in a different state than the
    shim's wake-up recipe produces, so the plan: the SHIM performs the full M2 recipe
    (HCE reset + DME_POWERON + DME_LINK_STARTUP + bDeviceInit), reads GPT, and only then
    hands the live link to the kernel — the kernel-side 0028 code stays untouched.
    A kernel-side fallback (port the recipe into ufs_take_over_host) only if the link
    does not survive the handoff.
  - Partition choice: `ufsroot=` boot-arg, default "userdata"; root device rd=ufsroot0.
    mkadt.py already builds the /sweet-ufs node (HC 0x1d84000); 0029 unsealed-root mounts
    the real iOS root from it.
  - The next flash should add `rd=ufsroot0` to the cmdline (mkpayload.py) to light up
    `SWEET UFS: "userdata" on LUN 0: first block ... read-only` on the panel — that is
    the M4 probe, no iOS root required yet.

### 2026-11-06 — M4 probe image built: shim wakes the UFS for the kernel, payload boots rd=ufsroot0

- The one kernel fact that shapes M4: `ufs_take_over_host` (xnu 0028, bsd/dev/sweet_ufs.c)
  REUSES a live link — it checks HCE/DP/UTRLRDY, re-points the transfer list at its own DMA
  block and TURs with 3 unit-attention retries. It has no UIC path, so it cannot bring the
  device up from the state the ABL leaves (bPowerMode=0, bBootLUNEn=0 — M2 proven). The
  wake therefore belongs to the shim, which owns the 19-round-proven recipe.
- shim/ufs.c refactored, no behavior change to the sequence: the M2 wake steps moved out of
  test_ufs into ufs_bringup() (MCQ->legacy, HCE reset, DME_POWERON, DME_LINK_STARTUP,
  transfer-list takeover, SET bDeviceInit + poll, NOP, TUR) — the same register writes in
  the same order, now callable from both paths. ufs_wake_for_kernel() is the M4 entry:
  gate checks (HCE set, doorbell clear) then ufs_bringup(), one red panel line per stage.
- shim/shim.c: the payload path now runs ufs_wake_for_kernel() BEFORE loader_boot() and
  prints `UFS WAKE final rc=`. A failed wake still boots XNU — the kernel's own probe
  line then tells us what it found, which is the diagnostic we want either way.
- tools: socrates.adt rebuilt (unchanged bytes — /sweet-ufs reg 0x1d84000/0x3000 was
  already there); payload rebuilt with `rd=ufsroot0` appended to the cmdline (mkpayload.py
  --cmdline). The kernel parses rd=ufsroot0 in IOFindBSDRoot -> sweet_ufs_rootdev ->
  ufsroot= boot-arg (default "userdata") picks the GPT partition by name.
- The packed image, shim/out-mac/xnu-boot.img, sha256 45323896c5d870009d9f423f6ba3b5ca…:
  embedded Image.gz byte-identical to the fresh build (v_height=1024 store present, the
  3200 encoding absent, 'wake for kernel' string in the binary); ramdisk byte-identical
  to the rd=ufsroot0 payload; ADT carries sweet-ufs (reg at node+0x88), fbcon 0x1194000,
  xiaomi,sweet compatible.
- Expected panel on the phone (all of it photographable, no serial):
  - red shim lines: the wake stages (UFS RST/DEVINI/NOP/TUR) + `UFS WAKE final rc=0`;
  - XNU clears the screen, then `SWEET UFS: "userdata" on LUN 0: first block 0x…, N MiB,
    read-only` + `BSD root: ufsroot0`, then a mountroot panic (userdata is f2fs, not
    HFS+) = M4 probe SUCCESS;
  - if the link does not survive the handoff: `SWEET UFS: host not ready (hce .. hcs ..)`
    then panic `SWEET UFS root ufsroot0 unavailable: 5` = the kernel-side fallback (port
    the recipe into the kernel) becomes the next step.
- QEMU smoke of the M4 probe (payload-qemu, rd=ufsroot0, 30/120/300 s): the kernel boots
  to the full SWEET driver set + heartbeat, then WEDGES in the kext-free IOKit quiet
  phase BEFORE IOFindBSDRoot - same screen (600-vsync counters frozen) at every
  duration, panic log empty, CPU taking timer IRQs. The M3 60 s run showed the same
  screen, so the wedge predates rd=ufsroot0 (cmdline-independent, QEMU-only so far:
  the phone's M3 photo proves these stages pass on real hardware). QEMU therefore
  CANNOT smoke-test the UFS root path - the flash is the probe's first real exercise.
  (The qemu-m3-ctrl control run panicked at the known fb site because it accidentally
  used the OLD apollofb payload - not comparable, discarded.)
- Repo: remotes consolidated to a single origin -> github.com/buzhaodi/ios-socrates
  (user 2026-11-06: all changes and the CI builds live there; the old apollo/ios-sweet
  remotes removed). README status table updated: M3 done on hardware, M4 in progress.

### 2026-11-06 — M4 flash #1 + the ROOT CAUSE of the pre-BSD park: 'qcom,kryo' vs 'qcom,kryo470'

- The M4 probe flash (image 45323896…) photo: kernel text to the full SWEET driver set
  + AppleCLCD + heartbeats at 10 s and 20 s — ALIVE (600 vsyncs, 0 skipped), no panic,
  but NO SWEET UFS / BSD root lines: IOFindBSDRoot never ran. The red UFS WAKE lines are
  invisible by design (fbcon clears the shim stage) — the wake itself is unproven either
  way; evidence for it should ride in the kernel's probe output next flash.
- The park reproduced EXACTLY in QEMU (30/120/300 s: driver set then heartbeats forever,
  panic log empty, CPU on timer IRQs) — so it was never a QEMU artifact but a payload bug.
- Root cause (code-reading): the socrates constants pass (95d275f) "corrected" the /cpus
  compatible from upstream's 'qcom,kryo470' to 'qcom,kryo'. 'kryo470' is NOT a hardware
  fact — it is the SweetCPU driver's match key (xnu 0018: IONameMatch='qcom,kryo470';
  apollo's real Mi 10T ADT ships the same string and boots to iOS). With 'qcom,kryo':
  no CPU nub matches -> startAll (xnu 0065) never runs -> gIOAllCPUInitializedKey never
  publishes -> the boot parks between the driver set and bsd_init FOREVER. QEMU run6's
  "60 s green" was green-but-parked; the M3 milestone (fbcon, drivers, vsync) stands,
  but the park hid everything after it.
- Fix: mkadt.py /cpus compatible back to 'qcom,kryo470' (comment in place). Both ADTs
  and payloads rebuilt.
- Proof, QEMU (qemu-m4-fix, 90 s): the kernel now reaches bsd_init and panics EXACTLY as
  designed: 'SWEET UFS: no /sweet-ufs node in the device tree' ->
  'panic(cpu 0): IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6' (the QEMU ADT
  has no UFS node - correct behavior there). On the phone, /sweet-ufs IS in the ADT, so
  the same path should print 'SWEET UFS: "userdata" on LUN 0: ...' instead.
- New flash image: shim/out-mac/xnu-boot.img sha cdc2ca4066f80955f121c70aabc09447aeb4053b
  e53a20a799e93b58926c8008 (shim wake + rd=ufsroot0 + kryo470 x8 verified inside).

### 2026-11-07 — kernel-side UFS wake built + flash #3 image (02cde07a…)

- CI round 1 (42242014697) failed 'a patch of ours was not applied': build.sh applies with
  git apply (STRICT, no fuzz) and 0030's hunk @-202 refused - my wake block sat between
  read_blocks and find_partition. validate-repo replayed with GNU patch -f, which fuzzed
  over it - a structural blind spot between the two tools. Fixed by moving
  ufs_query/ufs_dev_init to just before ufs_take_over_host; proven with a clean-room
  STRICT git-apply chain of all 186 patches (31 ufs sections, zero refused).
- CI round 2 (42245344012) GREEN. Kernel downloaded via gh + user proxy
  (127.0.0.1:10808), sha 78765a9e… verified against the artifact's SHA256SUMS. The
  binary carries the wake: 'host not ready (hce 0x%x hcs 0x%x), waking',
  'wake: link re-established (hcs 0x%x)', 'set fDeviceInit error %d',
  'fDeviceInit never cleared'.
- Flash #3 image shim/out-mac/xnu-boot.img sha 02cde07a…: shim wake (red lines) +
  kernel-side wake + rd=ufsroot0 + kryo470 x8 + /sweet-ufs + fbcon 18MB - verified
  inside the image. Both wake layers run: the shim's pre-XNU wake (whose link dies
  before bsd_init - flash #2 evidence) and now the kernel's own re-wake at probe time.
- Expected flash #3 panel (all photographable):
  1. XNU console to the SWEET driver set (as flash #2), then
  2. 'SWEET UFS: host not ready (hce 0x.. hcs 0x..), waking' -> 'wake: link
     re-established (hcs 0xB)' -> fDeviceInit SET/poll lines -> TUR -> GPT ->
     'SWEET UFS: "userdata" on LUN 0: first block 0x…, N MiB, read-only' ->
     'BSD root: ufsroot0' -> mountroot panic (userdata is f2fs) = M4 PROBE SUCCESS.
  3. If the kernel wake fails at a stage, its line names the stage exactly.

### 2026-11-07 — flash #3 verdict: the kernel wake RAN and named its failure; warm-reset fix built (flash #4 image 8405b864…)

- Flash #3 (02cde07a, 3-photo OCR): bsd_init -> Skywalk POST all PASS -> probe:
  'SWEET UFS: host not ready (hce 0x1 hcs 0x?), waking' ->
  'SWEET UFS: wake: HCE reset failed (hcs 0xB)'. The wake code executed, diagnosed
  itself, and the failure is a SEQUENCE error, not a hardware wall: on this boot's
  SECOND HCE reset (the shim already ran the full recipe pre-XNU and left the device
  mid-session), DP does NOT rise by itself after HCE=1 - it only returns with the
  UniPro link, i.e. AFTER DME_POWERON + DME_LINK_STARTUP. The original code polled DP
  BEFORE the UIC pair: a deadlock on warm reset. (The shim's first wake worked because
  the ABL's dead state is a different path: DP rises after HCE=1 there - M2 v18/v19.)
- Fix (commit a64aed8, CI 42250759235 green, kernel c02099eb…): ufs_wake_host now runs
  HCE reset -> UCRDY gate ((1u << 3) inline; a define would break 0034's hunk context
  again) -> DME_POWERON -> DME_LINK_STARTUP -> DP poll (1 s) -> the existing
  fDeviceInit/list path. Failure lines print hce alongside hcs.
- Flash #4 image shim/out-mac/xnu-boot.img sha 8405b864… (shim wake + warm-reset kernel
  wake + rd=ufsroot0 + kryo470 x8, verified inside).
- flash #4 expected panel: 'waking' -> 'wake: link re-established (hcs 0xB)' ->
  fDeviceInit set/poll -> TUR -> 'SWEET UFS: "userdata" on LUN 0: first block 0x…,
  N MiB, read-only' -> 'BSD root: ufsroot0' -> mountroot panic = M4 PROBE SUCCESS.
  Fallback diagnostics if not: 'controller not up (hce .. hcs ..)' = UCRDY never came
  (HCE write stuck / clock state); 'UIC 0x.. (is .. err ..)' = the UIC pair refused -
  then try hibernate-exit or skipping POWERON on warm reset; 'no device present after
  link startup' = link up but device silent.

### 2026-11-07 — flash #4 verdict: POWERON to a live device rejected; LINK_STARTUP-first order built (flash #5 image 7c5ee1b0…)

- Flash #4 (8405b864): the warm-reset fix WORKED - the wake passed the HCE reset +
  UCRDY gate and reached the UIC pair, where DME_POWERON was rejected:
  'SWEET UFS: wake: UIC 0x10 (is 0x404 err 0x80000001)' - a physical-adapter error.
  On the boot's second wake the device is ALREADY powered; POWERON to a live device
  is exactly what ufshcd never does (link recovery repeats LINK_STARTUP). The M2
  recipe's POWERON was proven only on the ABL's dead state.
- Fix (5492094, CI 42253265322 green, kernel dc0c0abb…): DME_LINK_STARTUP first (the
  warm path); on failure, DME_POWERON + LINK_STARTUP retry (the cold fallback, the M2
  recipe). Both handoff states covered without probing which one we are in.
- Flash #5 image shim/out-mac/xnu-boot.img sha 7c5ee1b00d06bb572a012b9d58bea2a50b3947
  95a0fe104f6e4df8385707db8e (shim wake + LINK_STARTUP-first kernel wake + rd=ufsroot0
  + kryo470 x8, verified inside).
- Flash #5 expectations: 'waking' -> (no UIC error expected) -> 'link re-established
  (hcs 0xB)' -> fDeviceInit set/poll -> TUR -> 'SWEET UFS: "userdata" on LUN 0: first
  block 0x…, N MiB, read-only' -> 'BSD root: ufsroot0' -> mountroot panic = M4 PROBE
  SUCCESS. If LINK_STARTUP is also rejected, the panel says so and hibernate-exit /
  SSU-first variants are the next candidates.

### 2026-11-07 — flash #5 verdict: BOTH UIC commands refused on wake #2; instrumented kernel built (flash #6 image 36ca84db…)

- Flash #5 (7c5ee1b0): fresh boot UUID (new kernel confirmed). The wake passed the HCE
  reset + UCRDY gate; the printout 'wake: UIC 0x10 (is 0x404 err 0x0)' decoded as: the
  0x10 is the FALLBACK - LINK_STARTUP (0x16) was tried first and refused, then POWERON's
  UIC ready bit never rose (err reads 0 because the starved command never latched an
  error). On the boot's second wake the UIC command layer itself stops serving.
- Flash #6 (instrumentation, kernel 60c1914b, CI 42320183268 green): ufs_uic_cmd prints
  'uic 0x%x starved (st .. is .. err ..)' per starved command, and the wake prints
  'link startup refused (is .. err ..)' between the attempts. The next photo will
  separate 'refused' (completed with an error) from 'starved' (ready bit never returned)
  for each of 0x16 and 0x10 - two different diseases with different fixes.
- Flash #6 image shim/out-mac/xnu-boot.img sha 36ca84dbafd4da0dc52a21f50ee53b9a1dba5e0
  2f1ee2294a259f0e2193cfccd.
- Parallel hypothesis ready if the UIC layer proves dead after the kernel's HCE reset:
  make the SHIM skip its pre-XNU wake (leave the ABL's raw handoff state for the kernel,
  the state where the M2 POWERON+LINK_STARTUP recipe is proven green) - a shim build
  flag away.

### 2026-11-07 — flash #6 verdict: BOTH UIC commands STARVED — the UIC layer wedges after the boot's SECOND HCE cycle; Plan B built (flash #7 d0ded551…)

- Flash #6 (36ca84db, instrumented): the wake passed HCE reset + UCRDY, then the photo
  carried the per-command verdicts (two OCR passes agree):
    'uic 0x16 starved (st 0x0 is 0x4   err 0x0)'   <- LINK_STARTUP
    'wake: link startup refused (is 0x4 err 0x0)'
    'uic 0x10 starved (st 0x0 is 0x404  err 0x0)'   <- POWERON fallback
  Not refused - STARVED: st (UIC status 0xA0) = 0 the whole poll: the ready bit dropped
  when the command was written and never rose; err 0x0 = no error ever latched. The
  controller saw the commands (IS flags moved) but its UIC layer never re-arms after
  this boot's SECOND HCE=0/1 cycle (the shim ran the first). That is a controller-level
  wedge, not a protocol error.
- Plan B (the pre-judged fallback): the shim's pre-XNU wake is now COMPILED OUT of the
  payload path (shim.c; NUFS_WAKE re-enables it for diagnostics). The kernel's wake
  becomes the boot's FIRST and ONLY HCE cycle - the ABL-handoff state where the M2
  recipe (HCE reset + POWERON + LINK_STARTUP + bDeviceInit) is proven green in 19
  rounds. Disassembly-verified: zero ufs_wake_for_kernel calls in the shim binary.
- Flash #7 image shim/out-mac/xnu-boot.img sha d0ded5512037e077440ca98d698e1e3b577ee0
  ce55ab9cef996e0d5a5b659221 (Plan B shim + the instrumented kernel + rd=ufsroot0 +
  kryo470 x8, verified inside). The kernel keeps the instrumented prints: if Plan B
  works, the panel shows the full success chain; if not, the starved/refused lines now
  describe the FIRST HCE cycle's behavior (a much more fundamental datum).

### 2026-11-07 — flash #7 = THE BREAKTHROUGH; 0187 built (flash #8 b6a8cf19…)

- Flash #7 (d0ded551, Plan B): NO wake lines at all - the ABL's handoff passed the
  kernel's ready gate directly (link UP at bsd_init time; Plan B was right, and flashes
  #2-#6 were all poisoned by the shim's own pre-XNU wake). The list takeover passed.
  TUR then CHECK-CONDITIONed on all 3 retries (response 0x1, status 0x2, printed by
  the kernel's command line) -> panic :5. That response/status pair is the M2
  first-flash disease: the device sits in needs-init state - bDeviceInit never set.
- The v19 cure, kernel-side, landed as patch 0187 (NOT in 0028: 0030/0034 patch that
  function's interior byte-for-byte; 0187 rides after the series, authored against the
  real 186-patch final tree with difflib, strict git-apply verified: 187 patches, zero
  refused). On TUR failure: ufs_dev_init() (SET fDeviceInit + poll) then the TUR loop
  again.
- CI 42322510617 green, kernel 30792767… Flash #8 image shim/out-mac/xnu-boot.img sha
  b6a8cf19b13e5a842666d3894acc4ea3334c301a83f32b19e72885c8ea22d588 (Plan B shim +
  0187 kernel + rd=ufsroot0 + kryo470 x8, verified inside).
- Flash #8 expected: (no wake) -> TUR fail x3 -> 'set fDeviceInit' path -> TUR GREEN ->
  GPT -> 'SWEET UFS: "userdata" on LUN 0: first block 0x…, N MiB, read-only' ->
  'BSD root: ufsroot0' -> mountroot panic = M4 PROBE SUCCESS.

### 2026-11-07 — flash #8 verdict: SET fDeviceInit rejected 0xFF (the already-done op — no
### HCE reset this boot, the ABL ran the bring-up); dev_init now reads the flag; flash #9
### image 9f92186f…

- Flash #8 (b6a8cf19): 0187 fired exactly as designed — TUR x3 CHECK CONDITION, then
  'SWEET UFS: set fDeviceInit error 255': SET_FLAG bDeviceInit returned GENERIC_FAILURE
  (0xFF). Root cause: with no HCE reset this boot (Plan B), the ABL already ran the
  device's bring-up during its own boot — the flag is ALREADY clear, and setting the
  already-done operation is rejected. The kernel aborted at the SET; the shim's v19
  logic reads the flag after ANY SET outcome — flag 0 = bring-up done, TUR proceeds.
- Fix (507d5d7): ufs_dev_init falls through to the READ poll on a SET failure
  ('set fDeviceInit error 255, reading the flag'). CI 42324294218 green (kernel
  8fc51080…). 187 patches validate + strict-chain green.
- Flash #9 image shim/out-mac/xnu-boot.img sha 9f92186fafdac335e8774ca43f16b676016f3ca4
  69 64af39078fc1e3385d9a4f (Plan B shim + 0187 + dev-init fallthrough + rd=ufsroot0 +
  kryo470 x8, verified inside).
- Flash #9 expected: TUR fail x3 -> 'set fDeviceInit error 255, reading the flag' ->
  (flag clear) -> TUR GREEN -> GPT -> 'SWEET UFS: "userdata" on LUN 0: first block
  0x…, N MiB, read-only' -> 'BSD root: ufsroot0' -> mountroot panic = M4 PROBE SUCCESS.

### 2026-11-07 — flash #9 verdict: the flag was CLEAR yet TUR still CHECK-CONDITIONs — the sense print built (flash #10 next)

- Flash #9 (9f92186f): the dev-init fallthrough worked as designed — 'set fDeviceInit
  error 255, reading the flag' -> the flag read CLEAR (no 'never cleared' line) ->
  dev_init returned 0 -> the retry TURs ran… and CHECK-CONDITIONed AGAIN (response 0x1,
  status 0x2, ocs 0, upiu 0x21 — the command pipeline itself is healthy). The
  fDeviceInit theory is dead: the device reports bring-up done and still refuses SCSI.
- Every CHECK CONDITION so far was read BLIND: the kernel's failure print had no sense
  bytes. The sense (key/ASC/ASCQ) is what names the disease (04/02 not-ready?
  05/24 invalid? 06/xx unit attention? 02/04...?). Fix (fd5d563): the failure print now
  includes slen/key/asc/ascq per the shim's dump_response offsets — the M2-proven
  layout. CI 42325979639 building.
- Cross-reference ready for flash #10: M2's v6/v16 rounds read sense 04/02 ('LOGICAL
  UNIT NOT READY - INITIALIZING COMMAND REQUIRED') on the ABL handoff — the cure there
  was SSU POWER_CONDITION=ACTIVE (v18's 'SSU rc=0' panel line). If flash #10's sense
  says 04/02 again, the fix is the kernel running SSU ACTIVE after the flag poll —
  the same dance the shim's v18 proved.

### 2026-11-07 — flash #10's sense (user-read) names the disease: key 2, ASC/ASCQ 04/02; the M2 v18 SSU cure goes kernel-side (flash #11 d7c9c811…)

- Flash #10 (99513bb8): the sense lines landed on the panel but the photo's UFS zone was
  glare-blinded; the user read the fields off the screen directly:
  'slen 0x12 key 0x2 asc/ascq 0x04/0x02' - key 2 NOT READY, ASC/ASCQ 04/02
  'LOGICAL UNIT NOT READY, INITIALIZING COMMAND REQUIRED'. The exact M2 v6/v16 disease.
- Cure (f2cf72c): the shim's M2 v18 SSU dance, kernel-side in 0187:
  START STOP UNIT with POWER_CONDITION = ACTIVE (cdb {0x1b,0,0,0,0x11,0}; the plain
  START_VALID form draws 05/24 - v17's lesson), printed as 'SSU ACTIVE rc 0', then a
  final TUR retry. 0187 now chains: TUR -> fDeviceInit (SET + flag poll) -> TUR ->
  SSU ACTIVE -> TUR -> find_partition. 187 patches validate + strict-chain green.
- CI 42327688341 green, kernel d57a4daf… Flash #11 image shim/out-mac/xnu-boot.img
  sha d7c9c811763a76c4998549121096072aade32b3dde687928ea697f944886b8aa (the SSU cdb
  bytes verified in the kernel image).
- Flash #11 expected: TUR fail (key 2 04/02) -> 'SSU ACTIVE rc 0' -> TUR GREEN -> GPT ->
  'SWEET UFS: "userdata" on LUN 0: first block 0x…, N MiB, read-only' -> 'BSD root:
  ufsroot0' -> mountroot panic = M4 PROBE SUCCESS.

### 2026-11-07 — flash #11 verdict: the SSU drew INVALID FIELD — the bBootLUNEn gate was missing; the full M2 order goes kernel-side (flash #12 4a652046…)

- Flash #11 (d7c9c811; user-read + close-up OCR): TUR x3 (key 2, 04/02) -> 'reading the
  flag' -> TUR x3 (04/02) -> 'command 0x1b failed ... asc 0x24' (INVALID FIELD IN CDB)
  -> 'SSU ACTIVE rc 5' -> panic :5. The SSU rejection is the M2 v16/v17 lesson
  resurfacing: the ABL disables the boot LU (bBootLUNEn=0) and the shim's green recipe
  re-enables it (WRITE_ATTR idn 0x00 = 1, the upiu_query_val bytes) BEFORE any SSU -
  flash #11's SSU drew INVALID FIELD because that gate was never set.
- Fix (0f89539): 0028 gains ufs_query_val (the attribute value in osf3 cmd[19..22],
  big-endian - the shim's upiu_query_val bytes); 0187 now chains the full M2 order:
  TUR -> fDeviceInit -> WRITE_ATTR bBootLUNEn=1 ('bBootLUNEn rc %d') -> SSU
  POWER_CONDITION=ACTIVE ('SSU ACTIVE rc %d') -> TUR -> GPT. 187 patches validate +
  strict git-apply chain green. CI 42329728345 green, kernel 269a3a0d…
- Flash #12 image shim/out-mac/xnu-boot.img sha 4a6520460843bbb51820c0d59535e9d90cd108
  557d6f31d2fd52a12c3a36662b (bBootLUNEn + SSU + dev-init + sense prints verified inside).
- Flash #12 expected: 'bBootLUNEn rc 0' -> 'SSU ACTIVE rc 0' -> TUR GREEN -> GPT ->
  'SWEET UFS: "userdata" on LUN 0: first block 0x…, N MiB, read-only' -> 'BSD root:
  ufsroot0' -> mountroot panic = M4 PROBE SUCCESS.

### 2026-11-07 — flash #12 verdict (full user transcript) — the ready gate was the miscalibration; the wake runs UNCONDITIONALLY (flash #13 421e1e74…)

- Flash #12 (4a652046; the user typed the ENTIRE panel): TUR x3 (key 2, 04/02) -> 'set
  fDeviceInit error 255, reading the flag' -> TUR x3 (04/02) -> 'bBootLUNEn = 255'
  (the WRITE_ATTR drew GENERIC_FAILURE) -> NO SSU line (skipped on the failed gate) ->
  panic :5.
- Re-reading the M2 campaign log corrected two misreadings:
  1. bBootLUNEn's WRITE was one of M2's FAILED experiments (v17: 'WRITE_ATTR
     bBootLUNEn rejected (GENERIC_FAILURE)') — same 255 — NOT part of the green recipe.
     The GREEN recipe: HCE reset + DME_POWERON + DME_LINK_STARTUP + bDeviceInit + NOP
     -> TUR rc=0.
  2. The 'ready gate' (skip the wake when hce/DP/UTRLRDY pass) was wrong: the ABL's
     handoff is LINK up but DEVICE powered down (bPowerMode 0 INACTIVE, the M2 v16
     read). 04/02 'INITIALIZING COMMAND REQUIRED' is the device asking for power —
     and every attribute write on a powered-down device draws GENERIC_FAILURE (both
     the fDeviceInit SET and the bBootLUNEn write).
- Fix (b0d8149, CI 42386950747 green, kernel 9fffc301…): ufs_take_over_host runs the
  wake UNCONDITIONALLY (gate removed; the wake is the boot's FIRST HCE cycle under
  Plan B — the M2-proven state; the #5/#6 UIC starve was a second-cycle disease).
  ufs_wake_host back to POWERON-then-LINK_STARTUP (the M2 cold order), live-device
  fallback retained. su.woke=true -> the existing dev_init/list/0187 chain follows.
- Flash #13 image shim/out-mac/xnu-boot.img sha 421e1e74e6e1ddd426a544f89c28114fcc6b
  4a3e9773890a7727394ba1a81fc0 (verified inside).
- Flash #13 expected: 'wake (hce 0x1 hcs 0xB)' -> 'wake: poweron rc 0' ->
  'wake: link re-established' -> fDeviceInit (rc 0 expected now — the HCE reset
  re-armed the need) -> NOP -> TUR GREEN -> GPT -> 'SWEET UFS: "userdata" on LUN 0:
  first block 0x…, N MiB, read-only' -> 'BSD root: ufsroot0' -> mountroot panic =
  M4 PROBE SUCCESS.

### 2026-11-07 — the review pivot: NO kernel-side HCE reset; the systematic pass (flash #14 building)

- The user's review set the strategy: flash #12 already had real SCSI-layer responses
  (04/02 with sense); flash #13's unconditional reset made it WORSE by failing earlier.
  The conclusion from all data: an HCE reset FROM THE KERNEL wedges the UIC command
  layer — flash #6 (second reset) AND flash #13 (FIRST reset; POWERON completed with
  PA 'already powered' 0x80000001, then everything starved) — while the ABL handoff
  state completes every query and SCSI command (flashes #7-#12). The kernel takes the
  link AS THE ABL LEFT IT; the wake is only the fallback for a dead handoff (gate
  restored in 0028, with the IS-ack + LINK_STARTUP-first fallback order retained).
- 0187 rewritten as the systematic M2 playbook on the no-reset state, everything
  dumped for one-photo diagnosis (f2b7a8c):
    attr idn 0x00/0x01 = …      (READ_ATTR: bBootLUNEn, bCurrentPowerMode)
    nop out rc N                (NOP OUT: the UFS-native link ping)
    report luns rc N luns U     (REPORT LUNS: the M2 v6 LUN-enumeration lesson)
    SSU ACTIVE rc N             (the SCSI 'initializing command' 04/02 asks for)
    ctl root probe failed: hce .. hcs .. is .. ucmd .. arg2 .. uecdl .. uecpa ..
                                (the Linux-ufshcd diagnosis axes: never executed /
                                completed with error / link negotiation failed)
- 'handshake structure not initialized' (the panic-log footer line) is a red herring -
  the UIC timeout is the explicit failure; per the review, do not chase it separately.
- CI 42388882077 building (kernel for flash #14). No rootfs / rd changes — the
  cmdline stays rd=ufsroot0; the probe is UFS-only, as the review ordered.

### 2026-11-08 — flash #14 image (the systematic pass, d0ca2216…)

- The review pivot landed (f2b7a8c + the decl fix df00c7a): no kernel-side HCE reset of a
  ready host (flashes #6/#13 proved it wedges the UIC layer); the ABL handoff link is
  taken over as-is; the wake stays only as the dead-handoff fallback. The root probe
  walks the M2 playbook on the no-reset state with EVERYTHING dumped for one photo:
  attr idn 0x00/0x01 = (bBootLUNEn, bCurrentPowerMode READs after flash #12's writes
  drew 255) -> nop out rc (the UFS-native link ping) -> report luns rc/luns (the M2 v6
  LUN-enumeration lesson) -> SSU ACTIVE rc -> TUR retries -> on failure 'ctl root
  probe failed: hce .. hcs .. is .. ucmd .. arg2 .. uecdl .. uecpa ..' (the Linux
  ufshcd diagnosis axes: never executed / completed with error / link negotiation).
- CI round 1 (42388882077) died on C99 use-before-declare: in the REAL 186-patch layout
  the probe calls live in sweet_ufs_init, and the helpers' anchor (before rootdev)
  landed after them; the hunk extraction also carried a stale 0147 context fragment.
  Fixed with fwd decls anchored before sweet_ufs_init + canonical helper text, and two
  NEW local gates before any kernel push: clang -fsyntax-only on the extracted new
  functions, and the decl-before-call check on the REAL 187-patch assembled file.
- CI 42390369176 green, kernel 1615817f… Flash #14 image shim/out-mac/xnu-boot.img
  sha d0ca22166af86b1275ae2d49b47d23372d08e876e8cde7b13ff59c8b25768b90 (all the
  systematic strings verified inside).
- Flash #14 panel = the full decision data set: attrs / nop / luns / SSU / ctl-dump,
  plus TUR's sense on every failure. One photo resolves the next move.

### 2026-11-08 — flash #14 verdict (systematic dump) → flash #15: the wake returns to the shim; the kernel handles a HIBERNATED link (db920a98…)

- Flash #14 (d0ca2216; user transcript) — the systematic dump resolved both open cases:
    attr idn 0x00 = 0x0        bBootLUNEn = 0 CONFIRMED (the READ works)
    attr idn 0x01 rc 253        bCurrentPowerMode idn does not exist here (0xFD)
    nop out rc 0                THE LINK + UTP PIPELINE ARE HEALTHY
    report luns rc 5            04/02 (the LU refuses I/O - not the link)
    SSU: key 0x5 asc/ascq 0x24  05/24 INVALID FIELD IN CDB
    ctl: hce 0x1 hcs 0x10f is 0x6 ucmd 0x1 uecdl 0 uecpa 0
  => The ABL handoff is EXACTLY M2's v17 wall (bBootLUNEn=0, writes 255, SSU 05/24,
     TUR 04/02) - and M2's only cure was the FULL wake recipe. The kernel cannot run
     it (kernel HCE resets wedge the UIC layer, flashes #6/#13) => THE WAKE RETURNS TO
     THE SHIM (Plan B inverted; the M2-proven environment).
  => The flashes #2-#6 'hcs 0x8 at kernel time' now have a mechanism: the shim-woken
     link HIBERNATES during the ~2.5 s of kernel bootstrap (DP drops in hibernate).
     The kernel's fallback wake is ufshcd's routine: DME_HIBERNATE_EXIT (0x11) ->
     DME_LINK_STARTUP (0x16), POWERON only as the powered-down fallback. NO HCE RESET
     anywhere in the kernel; the #14 systematic pass (attrs/nop/luns/SSU/ctl dumps)
     stays behind it.
- CI round for this had one compile-era lesson banked earlier (the 42388882077
  use-before-declare round): new local gates before kernel pushes now assemble the
  real 187-patch file, syntax-check the new functions with clang, and verify
  decl-before-call on the assembled file.
- CI 42392404120 green, kernel c4b47630…, shim rebuilt (f2d16f4a…, the mtime gate
  correctly demanded it). Flash #15 image shim/out-mac/xnu-boot.img sha db920a98a28ac4
  87ea6bfab25a52c8e72a62b2df53a39f934079bb9416df413f (shim wake + hib8-exit kernel
  wake + the systematic pass, verified inside).
- Flash #15 panel: red shim 'UFS WAKE final rc=0' (the M2 full recipe, red lines) ->
  XNU -> probe -> 'wake: hib8 exit rc 0' -> 'link startup rc 0' -> 'link
  re-established' -> fDeviceInit (rc 0 expected - the wake re-armed the need) -> NOP
  -> TUR GREEN -> GPT -> 'SWEET UFS: "userdata" on LUN 0' -> mountroot panic = M4
  PROBE SUCCESS. If the hibernate theory is wrong, the #14 dump lines appear again and
  the photo still decides.

### 2026-11-08 — flash #15 transcript: protocol errors found; flash #16 in preparation

- OBSERVED: user's full log shows HCE=1/HCS=8; UIC 0x11 first, then 0x16/0x10,
  all reported timeout, root panic errno60. Initial fastboot enumeration
  was empty; subsequent ADB check found socrates in fully-booted Android
  (boot_completed=1, uptime>599s). Device IS available for a temporary boot.
  UFS sysfs attribute reads are denied to the unprivileged adb shell.
- CONFIRMED by Linux v6.12 include/ufs/ufshci.h: 0x11 is DME_POWEROFF,
  HIBER_EXIT=0x18; 0xa0 is the end of the standard register block, not status.
  Poll HCS.UCRDY before dispatch, IS.UCCS after, ARG2[7:0] result; HIBER_EXIT
  additionally waits for IS.UHXS and HCS.UPMCRS=PWR_LOCAL. Preserve UIC errors.
  Thus #15 actively powered off UFS, and earlier "starved" diagnoses based
  on 0xa0 do NOT prove a kernel-only reset wedge or hibernation.
- 0188 also fixes UPIU queries per include/uapi/scsi/scsi_bsg_ufs.h and
  ufshcd.c: reserved byte4/LUN zero; value bytes20..23, flag byte23, power
  attribute IDN2. Old byte16 flag reads falsely claimed bring-up was done;
  old attribute reads do NOT establish bBootLUNEn=0 or inactive power.
  SSU ACTIVE CDB byte4=0x10 (START bit unsupported by UFS); REPORT LUNS
  allocation length encoded as16, matching its transfer buffer (was4096).
  Error registers: UECPA=0x38, UECDL=0x3c, UECDME=0x48.
- Shim cold bring-up now uses HCE reset -> UCRDY -> LINK_STARTUP -> fDeviceInit
  real flag poll -> NOP/TUR; disable AHIT so idle hibernate does not race the
  software handoff. Kernel retains no-reset takeover; timeout aborts before
  any queries or SCSI on an uninstalled transfer list.
- Local verification: 188 patches replay and validate; executable tests run
  the actual assembled C helpers against a mock controller (9 kernel + 4 shim UIC/query cases
  plus SCSI layout/early-abort checks); AArch64 shim builds with no warnings.
- CI 42394636091 stopped in the Linux regression harness before the kernel
  build: strict C11 did not declare BSD bzero. Fixed the harness with a
  portable memset wrapper (production code unchanged); rerun follows.
- Fix commits d2f8f3d/1266980; CI 42394687220 validation and full kernel
  build SUCCESS. Added actual-shim UIC mock execution (4 cases), total13 PASS.
  Local previous-kernel UUID exactly matches the user transcript; symbol table
  resolves the panic caller to IOFindBSDRoot.
- Flash #16 built and inspected: shim/out-mac/xnu-boot.img SHA256
  7eb9e9645b53889662319eaa3143997e59d490661d33f0046a067624823e14f4;
  kernel e2022d25f29dd8b77798eb82b184ae4107ea0289dea2259081c06befc987e46a,
  UUID D7B355EB-37AB-3C4C-A1B5-F4BD8C299869. Packed shim/DTB/payload exact
  byte matches verified; read-only cmdline preserved.
- Hardware attempt #16 OBSERVED: adb reboot bootloader from fully-booted
  Android; fastboot boot accepted 21388 KB (Sending OKAY0.494s, Booting
  OKAY0.102s, total0.603s). NO flash/erase commands or partition writes.
  No panel access from this Mac: UFS initialization/root-read outcome UNKNOWN,
  awaiting user photo/transcription. Bootloader acceptance is not M4 success.
- References: https://github.com/torvalds/linux/blob/v6.12/include/ufs/ufshci.h ;
  https://github.com/torvalds/linux/blob/v6.12/include/uapi/scsi/scsi_bsg_ufs.h ;
  https://github.com/torvalds/linux/blob/v6.12/drivers/ufs/core/ufshcd.c

### 2026-11-08 — flash #16 finds userdata; crash moves to unported CPU frequency (flash #17 preparation)

- User photo UUID matches the flash16 artifact. OBSERVED: Kernel data abort
  PC0xfffffe0007530dfc/LR0xfffffe0007530de0, ESR0x96000010,
  FAR0xfffffe31182a1100. Symbol table + disassembly resolve the PC exactly
  to sweet_cpufreq_init+0x118, `ldr w11,[x0,#0x100]`: first EPSS LUT read
  at physical0x18591100, the old SM8250 address. Socrates EPSS is elsewhere
  (fact bank0x17d91000), and its remaining driver details are not ported.
- Storage success is PROVEN by source control flow: CPU init is only called
  AFTER ufs_find_partition(userdata), block/character device registration,
  su.ready=true and the partition printf. The photo does not show the earlier
  UFS lines, but this call cannot be reached on a UFS lookup error.
  Root filesystem mount has NOT happened; Android userdata is untouched.
- Patch0189 stops unported legacy peripheral initialization on socrates after
  the UFS root is registered. Match the full bounded compatible list, since
  the first tokens are D22AP/iPhone10,3 and xiaomi,sweet is shared. Apollo
  continues its existing path. This isolates the M4 storage/root milestone;
  individual SM8550 peripherals remain later porting work.
- Local gates PASS:189-patch validation/replay, strict git apply0189,
  20 executable C mock cases (9 kernel UFS,4 shim,7 board token parsing),
  SCSI layouts and root-registration-before-board-gate ordering.
- User confirmed Android restarted/unlocked and USB connected. Commit08b23ef;
  CI42396141835 running, validation PASS.
- User clarified target scope: K60 Pro only; donor-device support is not a
  requirement for this project. No extra work should be spent preserving it.
- Android read-only inspection: userdata -> /dev/block/sda32; /data is f2fs
  through device mapper, confirming no iOS root has been installed yet.

### 2026-11-08 — sheng CPU-frequency reference checked at the user's request

- Upstream: UniXNU/UniXNU-sheng main commit `689400177f93219a38bd7cf5ae2e9f5782adc594`,
  package/CpuFreqProbe.cpp (363 lines; the archived socrates copy only had
  the older fixed-frequency modes, so upstream was checked directly).
  https://github.com/UniXNU/UniXNU-sheng/blob/689400177f93219a38bd7cf5ae2e9f5782adc594/package/CpuFreqProbe.cpp
- Useful SM8550 facts/implementation: domain windows0x17d91000/0x17d92000/
  0x17d93000; L3=0x17d90000, vote offset0x90; enable0x0, state0x20,
  DCVS control0xb0, frequency LUT0x100 (4-byte rows), performance state0x320.
  Per-core performance registers need votes when DCVS control bit0 is set.
  LUT selection excludes turbo rows, honors caps/floors and terminates at
  repeated frequencies; disabled clusters are deferred rather than voted.
- LIVE K60 Pro validation, read-only Android sysfs (not copied from sheng):
  policy0 related CPUs=0,1,2 (max2016000kHz); policy3 CPUs=3,4,5,6
  (max2803200kHz); policy7 CPU=7 (max3187200kHz). Correct topology3+4+1;
  old fact-bank4+3+1 was incorrect. Available lists also read successfully.
- Upstream mode4 governor:16ms windows, idle-derived busiest-core load,
  up immediately/down after4 windows, touch boost, GPU-frame floor and
  hardware throttling telemetry. These depend on sheng_cpu_idle_mach
  (recount), sheng HID/GPU counters and IOKit helpers; this XNU8796 tree uses
  different sample/boost APIs. This is a design/reference source, not a
  drop-in patch. Thermal sensor/control details still require K60 validation.
- Next CPU milestone: first adapt a default-off, read-only SM8550 EPSS/L3
  probe; verify real LUT/domain enable state, then conservative capped votes,
  then adapt load/temperature/boost policy. Keep flash17 storage isolation
  while that foundation is prepared; do not re-enable the Apollo governor.

### 2026-11-08 — sheng GPU/touch/Wi-Fi reuse audit; flash #17 CI green

- User expanded donor review to GPU, touch and Wi-Fi. LIVE K60 Pro Android
  observations: GPU sysfs reports Adreno740v2; dumpsys input identifies
  goodix_ts and touchscreen ranges0..1439.9/0..3199.9. WLAN device attaches
  below PCIe1c00000, endpoint0000:01:00.0. PCI vendor/device and firmware
  directories are denied to unprivileged adb, so exact WLAN silicon remains
  UNVERIFIED (do not promote the earlier WCN785x guess to fact).
- GPU: UniXNU/Adreno740 commit6fa7bae2a5b3b523e790eb3e0348af4d98969e4d,
  plus UniXNU/UniXNU-sm8550-common platform kits/ShengGmu.c and ShengGpuSmmu.c
  are strong low-level candidates: A740 command/ring/fence/scanout work plus
  SM8550 GMU, power/clock and SMMU handling. Board firmware, carveouts, DMA
  and display integration need K60 validation. AdrenoMetal commit
  6d7f73437087c57ee54a41adceb1735daaea405e uses metal2vulkan + Turnip, the
  same broad approach as this project's Metal2 stack; its user/kernel ABI
  and selected newer OS interfaces must be adapted to XNU8796/iOS16.
- Touch: sheng uses Novatek NT36532E (ShengNovatekTouch commit
  69fdd6223ed2572aa8bd8351e77c53a238276130), K60 uses Goodix. Do NOT copy its
  controller protocol, firmware, GPIO/pins or sensor geometry. Reuse candidate
  QcomGeniSpi commit eac47baefe6a5d2368a70351e6ad1b6361bedbe2 and HID bridge;
  get Goodix exact model/firmware/SPI engine/IRQ/reset from K60 vendor DT and
  driver sources before implementing the controller-specific path.
- Wi-Fi: Ath12kWifi commit28de7b91d7a880682b777e0e3564245a99dbc3f5 ports
  Linux ath12k + mac80211/cfg80211 and wpa_supplicant. Sheng board callbacks
  own power, PCIe, MSI, DMA, firmware and Apple80211 integration for WCN7850.
  This is a better candidate than assuming Apollo's ath11k can be retuned,
  IF K60 radio ID matches. Still need K60 board/calibration/firmware, PCIe
  resources and iOS16 network/userland ABI adaptation. No donor firmware was
  installed and no radio/power registers changed in this research pass.
- References: https://github.com/UniXNU/Adreno740 ;
  https://github.com/UniXNU/AdrenoMetal ; https://github.com/UniXNU/UniXNU-sm8550-common ;
  https://github.com/UniXNU/QcomGeniSpi ; https://github.com/UniXNU/ShengNovatekTouch ;
  https://github.com/UniXNU/Ath12kWifi
- Flash17: CI42396141835 SUCCESS, kernel commit08b23ef, SHA256
  21b7798cccf2de0c8197fc85f5cedffa01731d0e05f8f9d68c2c9f26c4393fff,
  UUID13B38CEA-2E65-3B80-8BA1-E1DA7873C5D5. Image shim/out-mac/xnu-boot.img
  SHA2568ba7285acb631e178468b7db3d57723f4850455c70d6e0f3792243f3d5723f0a.
  Packed shim/DTB/payload byte matches and new board-skip string verified;
  read-only boot arguments preserved.
- Hardware17 OBSERVED: adb reboot bootloader from user-confirmed unlocked
  Android; Sending21388KB OKAY0.493s, Booting OKAY0.102s, total0.600s.
  No flash/erase. Panel/mount result UNKNOWN pending user photo.
- Remaining CPU metadata issue for the next CPU/SMP milestone: mkadt.py still
  advertises old4+3+1 nominal clock assignments. Fix to the live3+4+1 and exact
  max frequencies before enabling SMP/governor. Flash17 keeps its identical
  prior ADT/cpus=1 to isolate the root-probe fix.

### 2026-11-08 — flash #17 panel: UFS storage/root-device probe SUCCESS

- User photo DIRECTLY CONFIRMS:
  `SWEET UFS: "userdata" on LUN 0: first block 0x2f7840, 229451 MiB, read-only`
  `SWEET UFS: socrates root ready; unported SM8250 peripherals skipped`
  `BSD root: ufsroot0B, major 1, minor 0`
  HFS unknown Volume Signature152e; hfs_mountfs error22, cannot mount root
  errno19; retries2..5 visible. The unported cpufreq data abort is gone.
- Milestone distinction: UFS/GPT/userdata read and BSD root-device registration
  are HARDWARE-PROVEN. M4 as a whole (iOS root/launchd/SpringBoard) is NOT done.
  The root data is still Android's partition content; no iOS HFS+ image was
  installed. The mount rejection is expected at this stage, not another UFS
  initialization failure. Root probe was read-only; Android partitions unchanged.
- Root preparation preflight: local IPSW iPhone10,3/iPhone10,6 version16.7.16
  build20H392 verified via BuildManifest, d22ap Erase identity. OS image
  087-86698-044.dmg (4730814044B), SystemOS087-86593-047.dmg (2743074816B),
  AppOS087-86715-047.dmg (10485760B); extraction COMPLETE, ZIP CRC validated
  and SHA256 recorded in research/ipsw/iPhone10_3-20H392/root-inputs.json
  (all Apple artifacts gitignored). OS hash80f426d5..., SystemOS9cd738f3...,
  AppOS391f2df3.... Mac has about87GiB free;
  Linux apfs-fuse/HFS utilities and Docker/Colima/Lima are absent.
- No backup/, recovery/ or research/ios-root directory found in this checkout.
  This does not prove backups do not exist elsewhere. Asked user for existing
  backup location and available Linux/WSL environment; response pending.

### Next concrete actions
1. Root inputs ready. Locate/complete M0 partition backups and verify hashes.
   Confirm available Linux/WSL build host (user question pending).
2. Build/tweak/check the HFS+ iOS root and sparse image, preserve artifact
   hashes and review concrete write target `/dev/block/by-name/userdata`.
3. ONLY after explicit destructive-step user confirmation, write existing
   userdata (no GPT changes; keep bootloaders/recovery/modem/persist/backups).
4. Temporary boot with the installed iOS root, then launchd/display and the
   K60-only CPU/touch/GPU/Wi-Fi adaptations using the banked sheng references.
