/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Milestone 4 wake-up: ufs_wake_for_kernel() runs the M2-proven recipe
 * (HCE reset + DME_LINK_STARTUP + fDeviceInit) and leaves the
 * host with OUR transfer list installed and the link live, so the kernel's
 * ufs_take_over_host (bsd/dev/sweet_ufs.c, xnu 0028) only has to re-point the
 * list at its own DMA block on a warm, initialized device.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x01d84000UL	/* ufshc@1d84000, sweet live FDT */

#define REG_AHIT	0x18
#define REG_IE		0x24
#define REG_UIC_CMD	0x90
#define REG_UIC_ARG1	0x94
#define REG_UIC_ARG2	0x98
#define REG_UIC_ARG3	0x9c
#define IS_UCCS		(1u << 10)

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
}

/* Sense dump - the RESPONSE UPIU layout per struct utp_cmd_rsp
 * (include/ufs/ufs.h): header[0..11] (status at byte 7), residual
 * transfer count[12..15], reserved[16..31], sense_data_len[32..33],
 * sense_data[34..51] (key at sense[2], ASC at [12], ASCQ at [13]).
 * The v3/v4 dump printed [16..23] - reserved, hence the all-zero "sns". */
static void dump_response(const char *tag)
{
	const volatile uint8_t *r = ucd.resp;
	con_hex2(tag, "r0=", r[0] | r[1] << 8 | r[2] << 16 | r[3] << 24,
	    "r1=", r[4] | r[5] << 8 | r[6] << 16 | r[7] << 24);
	con_hex2(tag, "resid=", r[12] | r[13] << 8 | r[14] << 16 | r[15] << 24,
	    "slen=", r[32] | r[33] << 8);
	con_hex2(tag, "key=", r[36], "asc/ascq=", r[46] | r[47] << 8);
}

/* NOP OUT: the UFS-native ping. Completing with a NOP IN (0x22) proves the
 * link and the command pipeline without any SCSI. */
static uint32_t nop_upiu(void)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));
	ucd.cmd[0] = 0x00;			/* transaction type: NOP OUT */
	ucd.cmd[3] = 0x80 + ++tag;
	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | (1u << 24);	/* UFS storage, no data */
	utrl[0].dw2 = 0x0f;
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {
			ufs_wr(REG_UTRLCLR, ~1u);
			return 1;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return ucd.resp[0] == 0x20 ? 0 : 2;	/* NOP IN is 0x20 (0x22 is DATA IN) */
}

/* One QUERY UPIU. The UPIU transaction table per include/ufs/ufs.h (and the
 * v5 lesson: 0x04 is TASK REQ, not QUERY - the device silently dropped our
 * "queries" exactly as it should have): QUERY REQUEST = 0x16, QUERY
 * RESPONSE = 0x36; the header's query_function byte [4] is
 * UPIU_QUERY_FUNC_STANDARD_WRITE (0x81) for SET_FLAG and READ (0x01) for
 * READ_FLAG; the query body after the 12-byte header holds opcode/idn.
 * Returns 0 on a 0x36 with response code 0 (resp[12]), 1 on doorbell
 * timeout, 2 on a wrong type, 3 on a nonzero query response code. */
static uint32_t upiu_query(uint8_t opcode, uint8_t idn, uint8_t qfunc)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));
	ucd.cmd[0] = 0x16;			/* QUERY REQUEST */
	ucd.cmd[2] = 0x00;			/* device management, no SCSI LUN */
	ucd.cmd[3] = ++tag;
	/* The canonical UPIU header (include/uapi/scsi/scsi_bsg_ufs.h):
	 * byte 4 = command_set_type (low nibble) | iid, byte 5 = query_function.
	 * v7 wrote qfunc into byte 4 - a garbage command set type, dropped. */
	ucd.cmd[4] = 0x00;			/* reserved for query requests */
	ucd.cmd[5] = qfunc;			/* 0x81 STD WRITE / 0x01 STD READ */
	ucd.cmd[12] = opcode;
	ucd.cmd[13] = idn;
	uintptr_t ucd_pa = (uintptr_t)&ucd;
	/* Linux sends every UTRD - SCSI, NOP and query alike - with
	 * command_type = UTP_CMD_TYPE_UFS_STORAGE (DW0 [31:28] = 1;
	 * ufshcd_setup_dev_cmd never overrides prepare_req_desc_hdr's
	 * default). v9's DEVICE_MANAGEMENT (2) drew silence too; back to 1. */
	utrl[0].dw0 = (1u << 28) | (1u << 24);	/* UFS storage, no data */
	utrl[0].dw2 = 0x0f;
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {
			ufs_wr(REG_UTRLCLR, ~1u);
			return 1;
		}
	}
	/* v10 evidence: OCS was still the 0x0F sentinel after the doorbell
	 * cleared - the controller accepts the doorbell immediately but the
	 * UTRD only completes when OCS is written back. SCSI and NOP finish
	 * fast enough to read in time; a query goes through the device
	 * management layer and takes longer. Poll OCS off the sentinel. */
	while ((utrl[0].dw2 & 0xff) == 0x0f) {
		if (ticks() - start > 19200000 * 5) {	/* 5 s */
			con_hex2("UFS Q", "ocs stuck 0F, is=", ufs_rd(REG_IS), "db=", ufs_rd(REG_UTRLDBR));
			ufs_wr(REG_UTRLCLR, ~1u);
			return 1;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("UFS Q", "ocs=", utrl[0].dw2 & 0xff, "is=", ufs_rd(REG_IS));
	if (ucd.resp[0] != 0x36)
		return 2;
	/* The query response code lives in the header at [6]; [12] echoes the
	 * opcode (0x06 for SET_FLAG, always nonzero). v12's rc=3 was a phantom.
	 * 0xFF = UPIU_QUERY_RESP_GENERIC_FAILURE (enum ufs_query_response). */
	return ucd.resp[6];		/* 0 = success, nonzero = the response code */
}

/* WRITE_ATTR: the 4-byte big-endian query value is at [20..23]. */
static uint32_t upiu_query_val(uint8_t opcode, uint8_t idn, uint8_t qfunc, uint32_t value)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));
	ucd.cmd[0] = 0x16;
	ucd.cmd[2] = 0x00;
	ucd.cmd[3] = ++tag;
	ucd.cmd[4] = 0x00;
	ucd.cmd[5] = qfunc;
	ucd.cmd[12] = opcode;
	ucd.cmd[13] = idn;
	ucd.cmd[20] = value >> 24;
	ucd.cmd[21] = value >> 16;
	ucd.cmd[22] = value >> 8;
	ucd.cmd[23] = value;
	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | (1u << 24);
	utrl[0].dw2 = 0x0f;
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {
			ufs_wr(REG_UTRLCLR, ~1u);
			return 1;
		}
	}
	while ((utrl[0].dw2 & 0xff) == 0x0f) {
		if (ticks() - start > 19200000 * 5) {
			ufs_wr(REG_UTRLCLR, ~1u);
			return 1;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("UFS Q", "ocs=", utrl[0].dw2 & 0xff, "is=", ufs_rd(REG_IS));
	if (ucd.resp[0] != 0x36)
		return 2;
	return ucd.resp[6];
}

/* The M2-proven wake-up recipe, step by step. Each stage returns 0 on success
 * and prints its own panel line, so a photo reads exactly where it died.
 * Shared by test_ufs (probe) and ufs_wake_for_kernel (M4 boot path). */
static uint32_t ufs_hce_reset(void)
{
	ufs_wr(REG_UTRLRSR, 0);			/* stop the transfer list */
	ufs_wr(REG_HCE, 0);			/* disable host controller */
	uint64_t t = ticks();
	while (ticks() - t < 1920000)			/* 100 ms */
		;
	ufs_wr(REG_HCE, 1);			/* enable host controller */
	t = ticks();
	while (!(ufs_rd(REG_HCS) & HCS_UCRDY) && ticks() - t < 19200000)
		;
	con_hex2("UFS RST", "hcs=", ufs_rd(REG_HCS), "hce=", ufs_rd(REG_HCE));
	if (!(ufs_rd(REG_HCS) & HCS_UCRDY))
		return 1;
	return 0;
}

/* UFSHCI command acceptance is HCS.UCRDY; completion is IS.UCCS.
 * 0xa0 is outside the standard register block, not a ready register. */
static uint32_t ufs_uic_cmd(uint32_t cmd)
{
	uint64_t t = ticks();
	while (!(ufs_rd(REG_HCS) & HCS_UCRDY)) {
		if (ticks() - t > 19200000)
			return 1;
	}
	ufs_wr(REG_IS, IS_UCCS);
	ufs_wr(REG_UIC_ARG1, 0);
	ufs_wr(REG_UIC_ARG2, 0);
	ufs_wr(REG_UIC_ARG3, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(REG_UIC_CMD, cmd);
	t = ticks();
	while (!(ufs_rd(REG_IS) & IS_UCCS)) {
		if (ticks() - t > 19200000) {
			con_hex2("UFS UIC timeout", "cmd=", cmd, "is=", ufs_rd(REG_IS));
			return 1;
		}
	}
	uint32_t result = ufs_rd(REG_UIC_ARG2) & 0xff;
	con_hex2("UFS UIC", "cmd=", cmd, "result=", result);
	ufs_wr(REG_IS, IS_UCCS);
	return result ? 2 : 0;
}

/* Point the host at OUR transfer list (must be called after every HCE reset:
 * the reset clears UTRLBA/UTRLBAU). Refuses to continue if the host did not
 * take the base, so no doorbell ever rings on the bootloader's stale list. */
static uint32_t ufs_take_list(void)
{
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return 1;
	}
	return 0;
}

/* The full recipe, from whatever state ABL left, to a warm device that
 * accepts SCSI on LUN 0 with bDeviceInit clear. Returns 0 on success. */
static uint32_t ufs_bringup(void)
{
	/* UFSHCI 4.0 (SM8550): the ABL may hand us the host in MCQ mode -
	 * REG_UFS_MEM_CFG 0x300 bit 0 = MCQ_MODE_SELECT (include/ufs/ufshci.h).
	 * In MCQ mode the legacy doorbell auto-completes with synthesized
	 * UPIU-shaped responses: our first-flash TURs came back instantly with
	 * the tag echoed, SCSI status 2 and NO sense bytes at all (and the NOP
	 * OUT got a RESPONSE UPIU instead of a NOP IN). Switch back to legacy.
	 */
	ufs_wr(REG_IE, 0);
	ufs_wr(REG_AHIT, 0);
	(void)ufs_rd(REG_AHIT);
	uint32_t mem_cfg = ufs_rd(0x300);
	con_hex2("UFS MODE", "mem_cfg=", mem_cfg, "mcq_cfg=", ufs_rd(0x380));
	if (mem_cfg & 1) {
		ufs_wr(0x300, mem_cfg & ~1u);
		con_hex2("UFS MODE", "->legacy=", ufs_rd(0x300), "was=", mem_cfg);
	}

	uint32_t rc;
	if ((rc = ufs_hce_reset()))
		return 10 + rc;
	/* A newly enabled UFSHCI host needs LINK_STARTUP, not a blind
	 * POWERON on an already-powered UniPro layer (Linux ufshcd sequence). */
	if ((rc = ufs_uic_cmd(0x16))) {
		con_hex2("UFS RST", "LINK hcs=", ufs_rd(REG_HCS), "dme=", ufs_rd(0x48));
		return 30 + rc;
	}
	con_hex2("UFS RST", "LINK hcs=", ufs_rd(REG_HCS), "dme=", ufs_rd(0x48));
	if (!(ufs_rd(REG_HCS) & HCS_DP))
		return 32;
	if ((rc = ufs_take_list()))
		return 40 + rc;

	/* fDeviceInit: the device accepts NOPs but refuses SCSI commands until
	 * the host sets the DeviceInit flag (SET_FLAG query, idn 0x01) and the
	 * device clears it when its bring-up is done - ufshcd's
	 * ufshcd_dev_link_startup does this exact dance. v19: ALWAYS set after
	 * the HCE reset - the reset puts the device back into "needs init" even
	 * when the flag reads 0 (v18's skip kept TUR at 04/02). */
	rc = upiu_query(0x05, 0x01, 0x01);	/* READ_FLAG bDeviceInit */
	con_hex2("UFS DEVINI", "read rc=", rc, "flag=", ucd.resp[23]);
	rc = upiu_query(0x06, 0x01, 0x81);	/* SET_FLAG bDeviceInit */
	con_hex2("UFS DEVINI", "set rc=", rc, "resp0=", ucd.resp[0]);
	/* The device clears bDeviceInit when its bring-up is done. */
	for (int i = 0; i < 20 && !rc; i++) {
		uint64_t t = ticks();			/* 50 ms at 19.2 MHz */
		while (ticks() - t < 960000)
			;
		rc = upiu_query(0x05, 0x01, 0x01);
		if (!rc && !ucd.resp[23])
			break;
	}
	con_hex2("UFS DEVINI", "poll rc=", rc, "flag=", ucd.resp[23]);
	if (rc || ucd.resp[23])
		return 50;

	/* Prove the pipeline: NOP IN, then TUR (unit attention tolerated). */
	rc = nop_upiu();
	con_hex2("UFS NOP", "rc=", rc, "resp0=", ucd.resp[0]);
	if (rc)
		return 60;
	static const uint8_t tur[6] = { 0x00 };
	uint32_t ocs = 0, st = 0;
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}
	if (rc)
		return 70;
	return 0;
}

/* M4 boot path: wake the device up for the kernel and leave OUR transfer
 * list installed (the kernel re-points it at its own DMA block; it only
 * needs HCE/DP/UTRLRDY, a live link and bDeviceInit clear). Prints one line
 * per stage on the red loader screen; returns 0 when the link is warm. */
uint32_t ufs_wake_for_kernel(void)
{
	con_str("UFS", "wake for kernel (M2 recipe)");
	uint32_t hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	if (!(hce & 1)) {
		con_str("UFS", "host disabled: cannot wake");
		return 1;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return 2;
	}
	uint32_t rc = ufs_bringup();
	con_hex2("UFS WAKE", "rc=", rc, "hcs=", ufs_rd(REG_HCS));
	return rc;
}

void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* The M2 recipe: MCQ->legacy, HCE reset, DME_LINK_STARTUP,
	 * transfer-list takeover, bDeviceInit, NOP, TUR (see ufs_bringup). */
	if (ufs_bringup()) {
		con_str("UFS", "bringup failed (see stages above)");
		return;
	}
	/* REPORT LUNS: the standard SCSI enumeration first command (SPC-5,
	 * opcode 0xa0, 12-byte CDB, allocation length at bytes 6-9 BE).
	 * The 04/02 sense on a bDeviceInit-clear device is LUN-level:
	 * the host must enumerate the LUNs before addressing them. */
	uint32_t ocs = 0, st = 0, rc;
	static uint8_t rl_buf[16];
	static const uint8_t rl[12] = { 0xa0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0 };
	rc = scsi(0, rl, sizeof(rl), rl_buf, 16, &ocs, &st);
	con_hex2("UFS RLUNS", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc == 4)
		dump_response("UFS RLUNS");
	if (!rc) {
		uint32_t nluns = rl_buf[3] / 8;
		con_hex2("UFS RLUNS", "luns=", nluns, "first=", rl_buf[8] | rl_buf[9] << 8);
	}

	/* The v6 TUR sense: key 2 (NOT READY), ASC/ASCQ 04/02 "LOGICAL UNIT NOT
	 * READY - INITIALIZING COMMAND REQUIRED" (SPC-5). The initializing
	 * command is START STOP UNIT: opcode 0x1b, START bit = byte 4 bit 0. */
	/* Device attributes via the (now proven) query channel: bBootLUNEn
	 * (idn 0x00) gates whether the boot LU accepts I/O; bPowerMode (idn 0x02)
	 * is ACTIVE(1)/SLEEP(2)/... — both tell us WHY the SCSI LUN is 04/02. */
	for (int idn = 0; idn < 3; idn++) {
		uint32_t rc = upiu_query(0x03, idn, 0x01);	/* READ_ATTR */
		/* the 4-byte attribute value comes in the query response body */
		con_hex2("UFS ATTR", "idn=", idn, "rc=", rc);
		con_hex2("UFS ATTR", "r0=", ucd.resp[0] | ucd.resp[1] << 8 | ucd.resp[2] << 16 | ucd.resp[3] << 24,
		    "r1=", ucd.resp[4] | ucd.resp[5] << 8 | ucd.resp[6] << 16 | ucd.resp[7] << 24);
		/* Query value is a BE32 at bytes 20..23. */
		con_hex2("UFS ATTR", "val=", be32(ucd.resp + 20),
		    "body=", ucd.resp[23] | ucd.resp[17] << 8 | ucd.resp[18] << 16 | ucd.resp[19] << 24);
	}

	/* v16's panel: bBootLUNEn = 0 (boot LU disabled by the ABL) and
	 * bPowerMode = 0 (inactive). Re-enable the boot LU and force ACTIVE. */
	{
		uint32_t rc = upiu_query_val(0x04, 0x00, 0x81, 1);  /* WRITE_ATTR bBootLUNEn=1 */
		con_hex2("UFS SETBL", "rc=", rc, "resp0=", ucd.resp[0]);
		dump_response("UFS SETBL");
		rc = upiu_query(0x03, 0x00, 0x01);	/* re-READ bBootLUNEn */
		con_hex2("UFS SETBL", "read rc=", rc, "val=", be32(ucd.resp + 20));
	}

	/* SSU with POWER_CONDITION = ACTIVE (bits 7:4 of byte 4 = 1), not
	 * START_VALID (0) — the 05/24 INVALID FIELD IN CDB on every prior SSU
	 * points at the power condition field. */
	static const uint8_t ssu_a[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	rc = scsi(0, ssu_a, sizeof(ssu_a), 0, 0, &ocs, &st);
	con_hex2("UFS SSU_A", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc == 4)
		dump_response("UFS SSU_A");

	/* And the traditional SSU (START_VALID, START=1) after the re-enable. */
	static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 1, 0 };
	rc = scsi(0, ssu, sizeof(ssu), 0, 0, &ocs, &st);
	con_hex2("UFS SSU", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc == 4)
		dump_response("UFS SSU");

	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}
	if (rc == 4) {
		dump_response("UFS TUR");
		rc = scsi(1, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR1", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (rc == 4)
			dump_response("UFS TUR1");
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}
