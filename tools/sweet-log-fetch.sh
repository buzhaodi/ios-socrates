#!/bin/sh
# Pull the SWEET persistent console log straight out of userdata over
# fastboot (no recovery boot, no adb, no partition writes): fastboot fetch
# reads the requested partition range as an Android sparse image.
# Requires a fastboot with "fetch" (platform-tools 31+; 37.0.1 verified here)
# AND a bootloader that serves FETCH - the script reports clearly if not.
#
#   tools/sweet-log-fetch.sh [-s SERIAL] OUT.txt
#
# Reads ONLY the last 1025 * 4 KiB blocks of userdata (4 MiB ring + header).
# The decoder is proven byte-exact against the repo's own sparse image
# (ios16-root.simg -> e2f63a63…, 27813 chunks, 12 GiB).
set -eu

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
	echo "Usage: $(basename "$0") [-s SERIAL] OUT.txt"
	echo "Pull the 4 MiB SWEET console log ring from userdata via fastboot fetch."
	exit 0
fi

FB="fastboot"
if [ "${1:-}" = "-s" ]; then
	shift
	SERIAL=${1:?Error: -s requires a device serial}
	shift
	FB="fastboot -s $SERIAL"
fi

out=${1:?usage: sweet-log-fetch.sh [-s SERIAL] OUT.txt}
command -v fastboot >/dev/null 2>&1 || { echo "Error: fastboot not found in PATH" >&2; exit 1; }

[ "$($FB getvar product 2>&1 | grep -c 'product:')" -ge 1 ] \
	|| { echo "Error: no fastboot device (enter FASTBOOT: hold Power+Vol-)" >&2; exit 1; }

PART=userdata
LOG_BLOCKS=1025
BLK=4096
LOG_BYTES=$((LOG_BLOCKS * BLK))

part_bytes=$($FB getvar partition-size:$PART 2>&1 | sed -n 's/.*partition-size:'"$PART"': *//p' | tr -d '\r ')
[ -n "$part_bytes" ] || { echo "Error: fastboot did not report partition-size:$PART" >&2; exit 1; }
case "$part_bytes" in
	0x*|0X*) part_bytes=$((part_bytes)) ;;
esac
[ "$part_bytes" -gt "$LOG_BYTES" ] \
	|| { echo "Error: partition smaller than log area ($part_bytes)" >&2; exit 1; }
[ $((part_bytes % BLK)) -eq 0 ] \
	|| { echo "Error: partition size not 4 KiB aligned: $part_bytes" >&2; exit 1; }

off=$((part_bytes - LOG_BYTES))
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "fetching $LOG_BYTES bytes at offset $off of $PART..." >&2
if ! $FB fetch "$PART:$off" "$LOG_BYTES" "$tmp/log-area.simg"; then
	echo "Error: fastboot fetch refused (bootloader may not implement FETCH)" >&2
	echo "Fallback: tools/sweet-log-pull.sh via a recovery ADB shell." >&2
	exit 1
fi

raw="$tmp/log-area.bin"
if command -v simg2img >/dev/null 2>&1; then
	simg2img "$tmp/log-area.simg" "$raw"
else
	python3 - "$tmp/log-area.simg" "$raw" "$BLK" "$LOG_BYTES" <<'PYEOF'
import struct, sys
src, dst, blk, want = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
data = open(src, 'rb').read()
if struct.unpack_from('<I', data, 0)[0] != 0xED26FF3A:
    open(dst, 'wb').write(data)          # not sparse: bootloader returned raw
    sys.exit(0)
magic, major, minor, fhsz, chsz, blk_sz = struct.unpack_from('<IHHHHI', data, 0)
total_blks, total_chunks = struct.unpack_from('<II', data, 16)
if blk_sz != blk:
    sys.exit('sparse block size %d, expected %d' % (blk_sz, blk))
got = 0
with open(dst, 'wb') as f:
    pos = fhsz
    for _ in range(total_chunks):
        typ, csz, tsz = struct.unpack_from('<III', data, pos)
        body = pos + chsz
        n = csz * blk_sz
        if typ == 0xCAC1:                    # raw
            f.write(data[body:body + n]); pos = body + n
        elif typ == 0xCAC2:                  # fill: 4-byte pattern repeated
            f.write(data[body:body + 4] * (n // 4)); pos = body + 4
        elif typ == 0xCAC3:                  # don't care: zeros
            f.write(b'\0' * n); pos = body
        elif typ == 0xCAC4:                  # crc32: skip
            pos = body
        else:
            sys.exit('bad sparse chunk type %#x at %d' % (typ, pos))
        got += n
    if got < want:
        f.write(b'\0' * (want - got))   # tail fill chunks may be elided
PYEOF
fi

size=$(stat -f %z "$raw" 2>/dev/null || stat -c %s "$raw")
[ "$size" = "$LOG_BYTES" ] || echo "Warning: expected $LOG_BYTES bytes, got $size" >&2

mkdir -p "$(dirname "$out")"
python3 "$(dirname "$0")/sweet-log.py" "$raw" "$out"
