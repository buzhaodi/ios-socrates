#!/usr/bin/env python3
"""Minimal Android boot.img v0 packer (layout of beryllium's working recovery.img: base 0, os_version 0x2004063c).
usage: mkbootimg_v0.py KERNEL RAMDISK OUT [--dtb FILE] [--cmdline STR]
The DTB is appended to the kernel (Qualcomm ABL picks it by msm-id/board-id)."""
import struct, sys, hashlib, argparse
p = argparse.ArgumentParser()
p.add_argument("kernel"); p.add_argument("ramdisk"); p.add_argument("out")
p.add_argument("--dtb"); p.add_argument("--cmdline", default="console=null")
p.add_argument("--base", type=lambda x: int(x, 0), default=0)
a = p.parse_args()
PAGE = 4096
k = open(a.kernel, "rb").read()
if a.dtb:
    k += open(a.dtb, "rb").read()
r = open(a.ramdisk, "rb").read()
pad = lambda b: b + b"\0" * (-len(b) % PAGE)
cmd = a.cmdline.encode()
assert len(cmd) < 512
sha = hashlib.sha1(); 
for b, in ((k,), (r,)):
    sha.update(b); sha.update(struct.pack("<I", len(b)))
sha.update(struct.pack("<I", 0))
ident = sha.digest().ljust(32, b"\0")
hdr = struct.pack("<8s10I16s512s32s1024s", b"ANDROID!",
    len(k), a.base + 0x8000, len(r), a.base + 0x01000000, 0, a.base + 0x00f00000,
    a.base + 0x100, PAGE, 0, 0x2004063c, b"", cmd.ljust(512, b"\0"), ident, b"")
open(a.out, "wb").write(pad(hdr) + pad(k) + pad(r))
print(f"{a.out}: kernel {len(k)} ramdisk {len(r)}")
