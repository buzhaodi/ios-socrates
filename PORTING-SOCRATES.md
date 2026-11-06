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

### Next concrete actions
1. (user) M4 probe flash, image ~/gittest/ios-socrates/shim/out-mac/xnu-boot.img
   (sha 45323896…): full Android boot once, unlock, ~30 s, `fastboot boot` it, photograph
   the panel — red wake lines AND the kernel SWEET UFS line if it works.
2. (user) Photograph the lower half of the M2 panel — the partition list for the record.
3. (user+agent) M0 backups over a recovery image (OrangeFox/TWRP for socrates) - needs the
   phone at hand; nothing destructive.
4. (agent) M4 proper: build the iOS root (tools/build-ios-root.sh, iPhone X 16.7.16 IPSW,
   needs Linux/WSL + ~40 GB) and write it into userdata - ONLY with the user's explicit
   confirmation at that destructive step (AGENTS.md rule).
