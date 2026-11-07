#!/usr/bin/env python3
"""Run real series-assembled UFS helpers against a mock UFSHCI controller.

No device required. C functions are extracted from the current source, not
reimplemented. The mock completes via the standard IS/ARG2 registers only.
Run: python3 tests/test_socrates_ufs.py
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'static (?:int|uint32_t|void)\s+\n?' + name + r'\([^;]*?\)\s*\{', source)
    if not match:
        raise AssertionError(name)
    start = match.start()
    pos = source.index('{', match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define bzero(p, n) memset((p), 0, (n))
#define __builtin_arm_dsb(x) ((void)0)
#define ETIMEDOUT 60
#define EIO 5
#define ENXIO 6
#define NSEC_PER_SEC 1000000000ull
static uint32_t regs[256], writes, mode, delays, result_code, query_count;
static uint64_t clock_ticks;
static unsigned char dma[0x110000];
static struct { uintptr_t dma; uint64_t dma_pa; unsigned char tag; } su;
DEFINES
STRUCTURES
static uint32_t ufs_rd(uint32_t off) { assert(off != 0xa0); return regs[off/4]; }
static void ufs_wr(uint32_t off, uint32_t v) {
    assert(off != 0xa0);
    if (off == REG_IS) { regs[off/4] &= ~v; return; }
    regs[off/4] = v;
    if (off == REG_UIC_CMD) {
        assert(v != 0x11); /* POWEROFF must never masquerade as HIBER_EXIT */
        writes++;
        if (mode == 1) return; /* lost completion */
        regs[REG_IS/4] |= IS_UCCS;
        regs[REG_UIC_ARG2/4] = result_code;
        if (v == 0x18 && mode != 2) {
            regs[REG_IS/4] |= 1u << 5;
            regs[REG_HCS/4] = 0x10b;
        }
    }
    if (off == REG_UTRLDBR) {
        struct utrd *d = (void *)(su.dma + DMA_UTRL);
        struct ucd *u = (void *)(su.dma + DMA_UCD);
        assert(u->cmd[4] == 0 && u->cmd[2] == 0);
        query_count++;
        d->dw2 = 0;
        memset(u->resp, 0, sizeof(u->resp));
        u->resp[0] = 0x36;
        /* Flag stays set on the first READ, then clears. Byte 16 is zero. */
        if (u->cmd[12] == 5) u->resp[23] = query_count < 3 ? 1 : 0;
        regs[off/4] = 0;
    }
}
static void delay(int us) { delays++; clock_ticks += us; }
static uint64_t mach_absolute_time(void) { return ++clock_ticks; }
static void nanoseconds_to_absolutetime(uint64_t n, uint64_t *t) { *t = n; }
static void dma_sync(uint32_t a, uint32_t b) { (void)a; (void)b; }
FUNCTIONS
static void reset(void) {
    memset(regs, 0, sizeof(regs)); memset(dma, 0, sizeof(dma));
    writes = mode = delays = result_code = query_count = 0;
    su.dma = (uintptr_t)dma; su.dma_pa = 0;
    regs[REG_HCS/4] = 8;
}
int main(void) {
    reset(); regs[REG_IS/4] = IS_UCCS | 4;
    assert(ufs_uic_cmd(0x16) == 0 && writes == 1);
    assert(regs[REG_IS/4] == 4); /* preserve diagnostic UIC_ERROR */
    reset(); mode = 1; regs[REG_IS/4] = IS_UCCS;
    assert(ufs_uic_cmd(0x16) == ETIMEDOUT); /* stale UCCS cannot pass */
    reset(); result_code = 1;
    assert(ufs_uic_cmd(0x16) == EIO);
    reset(); regs[REG_HCS/4] = 0;
    assert(ufs_uic_cmd(0x16) == ETIMEDOUT && writes == 0);
    reset(); assert(ufs_wake_host() == 0 && writes == 1);
    reset(); mode = 2;
    assert(ufs_uic_cmd(0x18) == ETIMEDOUT); /* missing asynchronous exit */
    reset(); mode = 1;
    assert(ufs_wake_host() == ETIMEDOUT && writes == 1);
    reset(); assert(ufs_dev_init() == 0 && query_count == 3);
    reset(); assert(ufs_query_val(4, 0, 0x81, 0x12345678) == 0);
    struct ucd *u = (void *)(su.dma + DMA_UCD);
    assert(u->cmd[19] == 0 && u->cmd[20] == 0x12 && u->cmd[21] == 0x34 &&
        u->cmd[22] == 0x56 && u->cmd[23] == 0x78);
    puts("UFS: 9 register/timeout/query regression cases passed");
}
'''


def main():
    with tempfile.TemporaryDirectory() as work:
        subprocess.run(['python3', str(ROOT/'tools/xnu-series-file.py'), work,
                        'bsd/dev/sweet_ufs.c'], check=True)
        source = (Path(work)/'sweet_ufs.c').read_text()
        defines = '\n'.join(line for line in source.splitlines()
                            if line.startswith('#define ') and
                            re.match(r'#define (REG_|HCS_|IS_|DMA_|UFS_BLOCK|BOUNCE_|PRDT_|CMD_TIMEOUT)', line))
        structs = source[source.index('struct utrd {'):source.index('static struct {')]
        funcs = '\n'.join(function(source, name) for name in
                          ['ufs_uic_cmd', 'ufs_wake_host', 'ufs_query', 'ufs_query_val', 'ufs_dev_init'])
        c = HARNESS.replace('STRUCTURES', structs).replace('DEFINES', defines).replace('FUNCTIONS', funcs)
        path = Path(work)/'test.c'
        path.write_text(c)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path), '-o', work+'/test'], check=True)
        subprocess.run([work+'/test'], check=True)
        # Wire-format bounds for the SCSI commands (kernel + shim diagnostics).
        shim = (ROOT/'shim/ufs.c').read_text()
        for code in [source, shim]:
            for opcode, count, byte, expected in [(0xa0, 12, 9, 16), (0x1b, 6, 4, 0x10)]:
                arrays = re.findall(r'\[\d+\]\s*=\s*\{\s*' + hex(opcode) + r',([^}]+)\}', code)
                assert arrays, hex(opcode)
                # shim retains a separately named legacy START_VALID diagnostic.
                values = [[opcode] + [int(x.strip(), 0) for x in a.split(',') if x.strip()] for a in arrays]
                if opcode == 0xa0:
                    assert all(len(v) == count and v[byte] == expected and v[8] == 0 for v in values)
                else:
                    assert any(len(v) == count and v[byte] == expected for v in values)
                    assert all(v[byte] != 0x11 for v in values)
        assert source.index('ufs_dump_ctl("takeover failed")') < source.index('static const uint8_t tur[6]', source.index('sweet_ufs_init(void)'))
        print('UFS: REPORT LUNS / SSU wire layouts and takeover failure guard passed')


if __name__ == '__main__':
    main()
