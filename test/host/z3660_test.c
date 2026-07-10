/*
 * z3660_test.c -- host unit test for src/z3660.c CDB synthesis.
 *
 * Compiles the REAL driver (src/z3660.c, -DHOST_TEST) against the mock piscsi
 * mailbox (mock_piscsi.c) and drives commands through z3660queue() with a
 * hand-built struct sdcom, completing via cp->intr (timeout() fires it
 * synchronously).  Asserts the driver's CD-ROM CDB bytes against the firmware's
 * established oracle (Z3660_emu/test/host/scsi_cd_test.cpp) and freezes the
 * disk path (pdt=0x00) -- the real box boots on that path.
 *
 * Exit code is gated ONLY by the mount-critical assertions (g_fail).  The
 * stretch CD<->oracle parity table (incl. MODE SENSE) is reported, never gated.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>			/* caddr_t (real system header) */

#include "mock_piscsi.h"

/* alien kernel ABI LAST: rico.h's short-name macros, then struct sdcom. */
#include "stubs/rico.h"
#include "stubs/sd.h"

extern bool z3660queue();		/* bool == char (rico.h); K&R decl */

/* CD units on this mock bus: 6 = CD-ROM (also the detection-probe unit), 0 = disk. */
#define U_CD	6
#define U_DISK	0
#define CD_BS	2048UL
#define CD_NBLK	0x00010000UL		/* 65536 blocks -> last LBA 0x0000FFFF */
#define DK_BS	512UL
#define DK_NBLK	0x00010000UL

/* ---- gating check (affects exit code) --------------------------------- */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("  FAIL: %s   [%s:%d]\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ---- stretch parity (reported, never gates) --------------------------- */
static int s_match = 0, s_dev = 0;
static void parity( name, matches, detail)
const char	*name, *detail;
int		matches;
{
	if (matches) { s_match++; printf("   [MATCH]     %-16s %s\n", name, detail); }
	else         { s_dev++;   printf("   [DEVIATION] %-16s %s\n", name, detail); }
}

/* ---- completion + one-shot command driver ----------------------------- */
static volatile int	g_done;
static uchar		g_status;
static char		g_okay;

static void test_intr( cp)
struct sdcom	*cp;
{
	g_status = cp->status;
	g_okay   = cp->okay;
	g_done   = 1;
}

static void run_cmd( unit, cdb, cdblen, data, nbyte)
int		unit, cdblen;
const uchar	*cdb;
void		*data;
unsigned	nbyte;
{
	struct sdcom	sc;

	memset( &sc, 0, sizeof sc);
	sc.unit  = (uint)unit;
	sc.addr  = (caddr_t)data;
	sc.nbyte = nbyte;
	memcpy( (void *)sc.cdb, cdb, (size_t)cdblen);
	sc.intr  = (void (*)())test_intr;

	g_done = 0;
	z3660queue( unit, &sc);
	if (!g_done) { g_fail++; printf("  FAIL: completion callback never fired\n"); }
	g_status = sc.status;
	g_okay   = sc.okay;
}

static unsigned long be32( p)
const uchar	*p;
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
	     | ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

static void dump( tag, b, n)
const char	*tag;
const uchar	*b;
int		n;
{
	int	i;
	printf("       %s:", tag);
	for (i = 0; i < n; i++) printf(" %02X", b[i]);
	printf("\n");
}

/* ====================================================================== */
/* GATING: CD-ROM (pdt=0x05) -- the cdfs mount path.                       */
/* ====================================================================== */

static void test_cd_inquiry()
{
	static const uchar exp[36] = {
		0x05,0x80,0x02,0x02,0x1F,0x00,0x00,0x00,
		'Z','3','6','6','0',' ',' ',' ',
		'A','M','I','X',' ','C','D','-','R','O','M',' ',' ',' ',' ',' ',
		'0','.','1',' '
	};
	uchar	cdb[6], buf[36];

	printf("[GATE] CD-ROM INQUIRY (pdt=0x05)\n");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x12; cdb[4] = 36;
	memset( buf, 0xEE, sizeof buf);
	run_cmd( U_CD, cdb, 6, buf, 36);
	dump( "INQUIRY", buf, 36);
	printf("       vendor='%.8s' product='%.16s' rev='%.4s'\n",
	       (char *)(buf + 8), (char *)(buf + 16), (char *)(buf + 32));
	CHECK( g_okay && g_status == 0, "CD INQUIRY completes GOOD");
	CHECK( memcmp( buf, exp, 36) == 0,
	       "CD INQUIRY == 05 80 02 02 1F 00*3 + 'Z3660   AMIX CD-ROM     0.1 '");
}

static void test_cd_read_capacity()
{
	uchar		cdb[10], buf[8];
	unsigned long	last_lba, blklen;

	printf("[GATE] CD-ROM READ CAPACITY(10)\n");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x25;
	memset( buf, 0xEE, sizeof buf);
	run_cmd( U_CD, cdb, 10, buf, 8);
	dump( "READ CAPACITY", buf, 8);
	last_lba = be32( buf);
	blklen   = be32( buf + 4);
	CHECK( g_okay && g_status == 0, "CD READ CAPACITY completes GOOD");
	CHECK( last_lba == CD_NBLK - 1, "CD READ CAPACITY last LBA == nblocks-1 (0x0000FFFF)");
	CHECK( blklen == 2048, "CD READ CAPACITY block length == 2048");
}

static void test_cd_read10()
{
	uchar		cdb[10], buf[16384];
	unsigned char	*back = mock_backing( U_CD);

	/* single sector @ LBA 0 */
	printf("[GATE] CD-ROM READ(10) single sector @ LBA 0\n");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[8] = 1;   /* 1 block */
	memset( buf, 0, 2048);
	run_cmd( U_CD, cdb, 10, buf, 2048);
	CHECK( g_okay && g_status == 0, "CD READ(10) LBA0 GOOD");
	CHECK( memcmp( buf, back + 0, 2048) == 0, "CD READ(10) LBA0 == backing[0..2048]");
	CHECK( be32( buf) == 0, "CD READ(10) LBA0 block marker == 0");

	/* multi-sector: 8 blocks @ LBA 0 -> 16384 bytes */
	printf("[GATE] CD-ROM READ(10) 8 sectors @ LBA 0 (16384 bytes)\n");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[8] = 8;
	memset( buf, 0, sizeof buf);
	run_cmd( U_CD, cdb, 10, buf, 16384);
	CHECK( g_okay && g_status == 0, "CD READ(10) 8-sector GOOD");
	CHECK( memcmp( buf, back + 0, 16384) == 0, "CD READ(10) 8-sector == backing[0..16384]");
	CHECK( be32( buf + 7 * 2048) == 7, "CD READ(10) 8-sector last block marker == 7");

	/* nonzero LBA: single sector @ LBA 5 -> proves block-index scaling */
	printf("[GATE] CD-ROM READ(10) single sector @ LBA 5 (offset scaling)\n");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[5] = 5; cdb[8] = 1;   /* LBA 5 */
	memset( buf, 0, 2048);
	run_cmd( U_CD, cdb, 10, buf, 2048);
	CHECK( g_okay && g_status == 0, "CD READ(10) LBA5 GOOD");
	CHECK( memcmp( buf, back + 5 * 2048, 2048) == 0,
	       "CD READ(10) LBA5 == backing[5*2048..] (block-index scaled)");
	CHECK( be32( buf) == 5, "CD READ(10) LBA5 block marker == 5 (not byte-addressed)");
}

static void test_cd_write_rejected()
{
	uchar	cdb[10], src[2048], sense[18], sense2[18], snap[2048];
	unsigned char	*back = mock_backing( U_CD);
	int	i, allzero;

	printf("[GATE] CD-ROM WRITE(10)/WRITE(6) rejected + sense\n");
	memcpy( snap, back + 0, 2048);			/* backing before */
	memset( src, 0x5A, sizeof src);

	/* WRITE(10) @ LBA 0, 1 block */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x2A; cdb[8] = 1;
	run_cmd( U_CD, cdb, 10, src, 2048);
	CHECK( g_status == 0x02, "CD WRITE(10) status == 0x02 (CHECK CONDITION)");
	CHECK( g_okay, "CD WRITE(10) okay==TRUE (framework CHECK-CONDITION signal)");
	CHECK( memcmp( back + 0, snap, 2048) == 0, "CD WRITE(10) leaves backing untouched");

	/* REQUEST SENSE -> DATA PROTECT */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x03; cdb[4] = 18;
	memset( sense, 0xEE, sizeof sense);
	run_cmd( U_CD, cdb, 6, sense, 18);
	dump( "SENSE", sense, 18);
	CHECK( sense[0] == 0x70, "REQUEST SENSE [0]==0x70 (current error, fixed fmt)");
	CHECK( sense[2] == 0x07, "REQUEST SENSE key [2]==0x07 (DATA PROTECT)");
	CHECK( sense[12] == 0x27, "REQUEST SENSE ASC [12]==0x27 (write protected)");
	CHECK( sense[13] == 0x00, "REQUEST SENSE ASCQ [13]==0x00");

	/* second REQUEST SENSE -> consumed, all zeros */
	memset( sense2, 0xEE, sizeof sense2);
	run_cmd( U_CD, cdb, 6, sense2, 18);
	allzero = 1;
	for (i = 0; i < 18; i++) if (sense2[i]) allzero = 0;
	CHECK( allzero, "second REQUEST SENSE all-zeros (consumed-on-read)");

	/* WRITE(6) @ LBA 0, 1 block */
	memcpy( snap, back + 0, 2048);
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x0A; cdb[4] = 1;
	run_cmd( U_CD, cdb, 6, src, 2048);
	CHECK( g_status == 0x02, "CD WRITE(6) status == 0x02 (CHECK CONDITION)");
	CHECK( memcmp( back + 0, snap, 2048) == 0, "CD WRITE(6) leaves backing untouched");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x03; cdb[4] = 18;
	memset( sense, 0xEE, sizeof sense);
	run_cmd( U_CD, cdb, 6, sense, 18);
	CHECK( sense[2] == 0x07 && sense[12] == 0x27 && sense[13] == 0x00,
	       "CD WRITE(6) sense == DATA PROTECT 07/27/00");
}

/* ====================================================================== */
/* GATING: disk regression (pdt=0x00) -- freeze current byte behavior.     */
/* ====================================================================== */

static void test_disk_inquiry_frozen()
{
	/* Frozen from the current driver (NOT the oracle's disk strings): the
	 * exact 36 bytes it emits for a direct-access disk.  Note the rev field
	 * reads " 0.1", not "0.1 " -- the disk id string "Z3660   PiSCSI Disk
	 * 0.1 " is one space longer than the CD's, so the 28-byte copy lands the
	 * rev one column left.  A pre-existing disk-path quirk; frozen as-is
	 * (the disk path is out of scope for this harness). */
	static const uchar exp[36] = {
		0x00,0x00,0x02,0x02,0x24,0x00,0x00,0x00,
		'Z','3','6','6','0',' ',' ',' ',
		'P','i','S','C','S','I',' ','D','i','s','k',' ',' ',' ',' ',' ',
		' ','0','.','1'
	};
	uchar	cdb[6], buf[36];

	printf("[GATE] DISK INQUIRY frozen (pdt=0x00)\n");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x12; cdb[4] = 36;
	memset( buf, 0xEE, sizeof buf);
	run_cmd( U_DISK, cdb, 6, buf, 36);
	dump( "INQUIRY", buf, 36);
	printf("       vendor='%.8s' product='%.16s' rev='%.4s'\n",
	       (char *)(buf + 8), (char *)(buf + 16), (char *)(buf + 32));
	CHECK( g_okay && g_status == 0, "DISK INQUIRY completes GOOD");
	CHECK( memcmp( buf, exp, 36) == 0, "DISK INQUIRY 36 bytes frozen (Z3660 / PiSCSI Disk / 0.1)");
}

static void test_disk_read_capacity_frozen()
{
	uchar		cdb[10], buf[8];

	printf("[GATE] DISK READ CAPACITY(10) frozen 512-byte blocks\n");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x25;
	memset( buf, 0xEE, sizeof buf);
	run_cmd( U_DISK, cdb, 10, buf, 8);
	dump( "READ CAPACITY", buf, 8);
	CHECK( be32( buf) == DK_NBLK - 1, "DISK READ CAPACITY last LBA == nblocks-1");
	CHECK( be32( buf + 4) == 512, "DISK READ CAPACITY block length == 512");
}

static void test_disk_read10_frozen()
{
	uchar		cdb[10], buf[2048];
	unsigned char	*back = mock_backing( U_DISK);

	printf("[GATE] DISK READ(10) frozen (512-byte blocks)\n");
	/* single sector @ LBA 0 */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[8] = 1;
	memset( buf, 0, sizeof buf);
	run_cmd( U_DISK, cdb, 10, buf, 512);
	CHECK( g_okay && g_status == 0, "DISK READ(10) LBA0 GOOD");
	CHECK( memcmp( buf, back + 0, 512) == 0, "DISK READ(10) LBA0 == backing[0..512]");

	/* 4 sectors @ LBA 8 -> 2048 bytes, nonzero LBA */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[5] = 8; cdb[8] = 4;
	memset( buf, 0, sizeof buf);
	run_cmd( U_DISK, cdb, 10, buf, 2048);
	CHECK( g_okay && g_status == 0, "DISK READ(10) LBA8 x4 GOOD");
	CHECK( memcmp( buf, back + 8 * 512, 2048) == 0, "DISK READ(10) LBA8 x4 == backing[8*512..]");
	CHECK( be32( buf) == 8, "DISK READ(10) LBA8 block marker == 8 (512-byte scaled)");
}

static void test_disk_write10_frozen()
{
	uchar		cdb[10], src[512];
	unsigned char	*back = mock_backing( U_DISK);
	int		i;

	printf("[GATE] DISK WRITE(10) permitted (pdt=0x00)\n");
	for (i = 0; i < 512; i++) src[i] = (uchar)(0xC0 + (i & 0x0F));
	/* write @ LBA 10 (distinct from the read-test LBAs) */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x2A; cdb[5] = 10; cdb[8] = 1;
	run_cmd( U_DISK, cdb, 10, src, 512);
	CHECK( g_okay && g_status == 0, "DISK WRITE(10) completes GOOD (0x00)");
	CHECK( memcmp( back + 10 * 512, src, 512) == 0, "DISK WRITE(10) lands in backing[10*512..]");
}

static void test_tur_both()
{
	uchar	cdb[6];

	printf("[GATE] TEST UNIT READY both device types\n");
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x00;
	run_cmd( U_CD, cdb, 6, (void *)0, 0);
	CHECK( g_okay && g_status == 0, "CD TEST UNIT READY -> GOOD");
	run_cmd( U_DISK, cdb, 6, (void *)0, 0);
	CHECK( g_okay && g_status == 0, "DISK TEST UNIT READY -> GOOD");
}

/* ====================================================================== */
/* STRETCH: CD parity vs the oracle's CD half (reported, NOT gated).       */
/* ====================================================================== */

static void stretch_cd_parity()
{
	uchar		cdb[10], buf[64];
	unsigned long	last_lba, blklen;
	int		ms10_fixed;

	printf("\n=== STRETCH: driver vs a3000_scsi CD oracle (scsi_cd_test.cpp) ===\n");

	/* INQUIRY (oracle: b0=0x05, b1=0x80, product 'AMIX CD-ROM     ') */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x12; cdb[4] = 36;
	memset( buf, 0xEE, sizeof buf);
	run_cmd( U_CD, cdb, 6, buf, 36);
	parity( "INQUIRY", buf[0] == 0x05 && buf[1] == 0x80
	        && memcmp( buf + 16, "AMIX CD-ROM     ", 16) == 0,
	        "b0=05 b1=80 product 'AMIX CD-ROM     '");

	/* READ CAPACITY (oracle: last LBA 0x0000FFFF, blklen 2048) */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x25;
	memset( buf, 0xEE, sizeof buf);
	run_cmd( U_CD, cdb, 10, buf, 8);
	last_lba = be32( buf); blklen = be32( buf + 4);
	parity( "READ CAPACITY", last_lba == 0xFFFF && blklen == 2048,
	        "last-LBA 0x0000FFFF, blklen 2048");

	/* MODE SENSE(6) (oracle: WP b2&0x80, mode-data-len b0==0x0B) */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x1A; cdb[2] = 0x3F; cdb[4] = 0x40;
	memset( buf, 0xEE, sizeof buf);
	run_cmd( U_CD, cdb, 6, buf, 64);
	dump( "MODE SENSE6 ", buf, 12);
	parity( "MODE SENSE(6)", g_okay && (buf[2] & 0x80) && buf[0] == 0x0B,
	        "WP set, len 0x0B (no rigid-geometry pages)");

	/* MODE SENSE(10) (oracle: WP b3&0x80, mode-data-len b0..1==0x00,0x0E).
	 * The base driver has NO 0x5A case -> default hard error.  This probe
	 * reports MATCH only once the CD-only stretch fix is in. */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x5A; cdb[2] = 0x3F; cdb[8] = 0x40;
	memset( buf, 0xEE, sizeof buf);
	run_cmd( U_CD, cdb, 10, buf, 64);
	dump( "MODE SENSE10", buf, 16);
	ms10_fixed = g_okay && g_status == 0 && (buf[3] & 0x80)
	           && buf[0] == 0x00 && buf[1] == 0x0E;
	if (ms10_fixed)
		parity( "MODE SENSE(10)", 1, "WP set, len 0x000E (CD stretch fix present)");
	else {
		char det[96];
		sprintf( det, "driver hard-errors 0x5A (okay=%d status=0x%02X b0..3=%02X %02X %02X %02X)",
		         (int)g_okay, g_status, buf[0], buf[1], buf[2], buf[3]);
		parity( "MODE SENSE(10)", 0, det);
	}

	/* WRITE gating already asserted GOOD above; restate for the table. */
	parity( "WRITE(10) gate", 1, "-> 0x02 CHECK CONDITION + DATA PROTECT 07/27/00");
	parity( "WRITE(6) gate", 1, "-> 0x02 CHECK CONDITION + DATA PROTECT 07/27/00");
}

/* ====================================================================== */
int main()
{
	mock_reset();
	mock_add_drive( U_CD,   0x05, CD_BS, CD_NBLK, 128UL * CD_BS);   /* 256 KB backing */
	mock_add_drive( U_DISK, 0x00, DK_BS, DK_NBLK, 256UL * DK_BS);   /* 128 KB backing */

	printf("=== Z3660 SCSI host CDB test (real src/z3660.c, -DHOST_TEST) ===\n\n");

	test_cd_inquiry();
	test_cd_read_capacity();
	test_cd_read10();
	test_cd_write_rejected();

	test_disk_inquiry_frozen();
	test_disk_read_capacity_frozen();
	test_disk_read10_frozen();
	test_disk_write10_frozen();

	test_tur_both();

	stretch_cd_parity();

	printf("\n=== GATING: %d passed, %d failed ===\n", g_pass, g_fail);
	printf("=== STRETCH parity: %d match, %d deviation ===\n", s_match, s_dev);
	return g_fail ? 1 : 0;
}
