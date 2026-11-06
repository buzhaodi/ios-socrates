#!/bin/bash
# M3 flash image: the shim (build-mac.sh's Image) + the XNU payload as the
# ramdisk (apollo's build.sh recipe: loader.c finds SWEETXNU in /chosen
# linux,initrd-start..end and boots it). Payload from tools/mkpayload.py.
set -euo pipefail
cd "$(dirname "$0")"
PAYLOAD=${1:-out/payload.bin}
[ -f "$PAYLOAD" ] || { echo "usage: build-mac-xnu.sh [payload.bin] (default ../out/payload.bin)" >&2; exit 1; }
DTB=../research/dtb/socrates-all-dtbs.dtb
[ -f "$DTB" ] || { echo "missing $DTB" >&2; exit 1; }
[ -f out-mac/Image.gz ] || { echo "run build-mac.sh first (out-mac/Image.gz)" >&2; exit 1; }

# Staleness gate (the first M3 flash, 2026-11-06, packed the pre-fix shim with
# the post-fix payload and the phone panicked on exactly the bug the fix fixed:
# pmap_map_bd, boot_args.Video.v_height=3200 in the old binary. QEMU never saw
# it because qemu-sweet.py builds boot_args itself and never runs this loader.)
newest_src=$(ls -t *.c *.S *.ld ../tools/mkbootimg_v0.py | head -1)
[ "out-mac/Image.gz" -nt "$newest_src" ] || {
  echo "error: out-mac/Image.gz is older than $newest_src - run build-mac.sh first" >&2; exit 1; }
newest_py=$(ls -t ../tools/mkadt.py ../tools/mkpayload.py | head -1)
[ "$PAYLOAD" -nt "$newest_py" ] || {
  echo "error: $PAYLOAD is older than $newest_py - rebuild the payload" >&2; exit 1; }

cp "$PAYLOAD" out-mac/ramdisk-xnu.bin
python3 ../tools/mkbootimg_v0.py out-mac/Image.gz out-mac/ramdisk-xnu.bin out-mac/xnu-boot.img \
  --dtb "$DTB" --cmdline "console=null"
shasum -a 256 out-mac/xnu-boot.img
echo "DONE: shim/out-mac/xnu-boot.img (M3: fastboot boot, after one full Android boot)"
