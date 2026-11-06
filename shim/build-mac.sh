#!/bin/bash
# Build the socrates shim + M1 boot image on macOS with homebrew clang+lld.
# apollo's build.sh uses aarch64-linux-gnu-gcc (Linux/WSL); the same sources
# build clean with clang once two clang-vs-gcc differences are handled:
#   1. clang's switch/lookup-table transform materializes string pointers as
#      R_AARCH64_ABS64 in .rodata - the shim forbids absolute relocations (the
#      ABL loads the image unrelocated). The exception-name switch is a
#      char[4][8] array instead (shim.c).
#   2. __builtin_strcmp (ufs.c) becomes a libc call under clang - freestanding,
#      so shim.c carries strcmp.
# The ABS64 gate and the _end padding mirror build.sh; _end parses via python
# (macOS awk has no strtonum).
# Output: out-mac/Image (the shim) and out-mac/shim-boot.img (M1 flash image:
# Image + the 8-block splash-injected socrates vendor DTB blob + empty newc
# ramdisk, Android boot header v0 - the header version the K60 Pro's ABL has
# accepted all along, mik60proios boot.json evidence).
# research/dtb/socrates-all-dtbs.dtb is the gitignored vendor blob produced by
# the mik60proios campaign (tools/inject-nodes-mac.sh: 8 blocks + splash).
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/bin:$PATH

DTB=../research/dtb/socrates-all-dtbs.dtb
[ -f "$DTB" ] || { echo "missing $DTB (copy it from the mik60proios campaign: ~/unixnu-work/out/socrates-all-dtbs.dtb)" >&2; exit 1; }

rm -rf out-mac && mkdir out-mac
CFLAGS="--target=aarch64-linux-gnu -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
  clang $CFLAGS -c $f -o out-mac/${f%.*}.o
done

# The ABS64 gate: C objects must be position-independent (start.o's header
# entry is the one allowed absolute).
abs=$(llvm-readelf -r out-mac/shim.o out-mac/console.o out-mac/mmu.o out-mac/fdt.o out-mac/loader.o out-mac/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }

clang --target=aarch64-linux-gnu -fuse-ld=lld -static -nostdlib -T shim.ld \
  -Wl,--build-id=none -Wl,-N \
  out-mac/start.o out-mac/shim.o out-mac/console.o out-mac/mmu.o out-mac/fdt.o out-mac/loader.o out-mac/ufs.o \
  -o out-mac/shim.elf
llvm-objcopy -O binary out-mac/shim.elf out-mac/Image
size=$(llvm-nm out-mac/shim.elf | python3 -c "import sys
for line in sys.stdin:
    p = line.split()
    if len(p) == 3 and p[2] == '_end':
        print(int(p[0], 16)); break")
truncate -s "$size" out-mac/Image
echo "shim Image: $size bytes"

# empty newc ramdisk (ABL dislikes a zero-size ramdisk). Deterministic: a
# trailer-only archive built in python (cpio embeds directory mtimes, which
# made the image sha change on every build).
python3 -c "
import gzip
def ent(name):
    f = [0] * 13
    f[11] = len(name) + 1
    hdr = ('070701' + '%08X' * 13 % tuple(f)).encode()
    return hdr + name.encode() + b'\0' + b'\0' * ((-len(name) - 1) & 3)
arc = ent('TRAILER!!!') + b'\0' * 1024
open('out-mac/ramdisk.bin', 'wb').write(gzip.compress(arc, 9, mtime=0))
"

# gzip the kernel: the K60 Pro's ABL accepts a raw Image with "OKAY" and then
# refuses to run it (first M1 flash, 2026-11-06: fastboot OKAY, black panel, no
# shim red screen). The mik60proios campaign's kernels were always
# gzip(<arm64 image>) with the dtb appended after the gzip stream, and
# beryllium hit the same wall on SDM845 ("a raw Image is accepted and then
# refused"). The ARM64 header (image_size = the _end) rides inside the gzip.
gzip -9nc out-mac/Image > out-mac/Image.gz
python3 ../tools/mkbootimg_v0.py out-mac/Image.gz out-mac/ramdisk.bin out-mac/shim-boot.img \
  --dtb "$DTB" --cmdline "console=null"
shasum -a 256 out-mac/shim-boot.img
echo "DONE: shim/out-mac/shim-boot.img (M1 flash: fastboot boot, after one full Android boot)"
