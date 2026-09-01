/*
 * z3660_test.c -- host unit test for src/z3660.c CDB synthesis.
 *
 * Compiles the REAL driver (src/z3660.c, -DHOST_TEST) against the mock piscsi
 * mailbox (mock_piscsi.c) and drives commands through z3660queue() with a
 * hand-built struct sdcom.  Completion is delivered IN-CONTEXT by the driver's
 * own z3660_complete() before z3660queue() returns (a5af58a removed the timeout()
 * deferral), so a test reads its result straight afterwards.  Asserts the
 * driver's CD-ROM CDB bytes against the firmware's established oracle
 * (Z3660_emu/test/host/scsi_cd_test.cpp) and freezes the disk path (pdt=0x00) --
 * the real box boots on that path.
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

/* permanent reentrancy counters exported by the driver (kmem-readable on HW) */
extern unsigned long z3660_nest_depth, z3660_nest_hits;

/* which mapping arm z3660map() took: 1 = section-0 identity, 0 = sptalloc'd */
extern unsigned char z3660_direct_map;

extern int	z3660present();		/* sd.c probe hook (K&R decl) */
static char	*g_probe_base;		/* board base z3660present() reports */

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
/* GATING: which mapping arm ran.                                          */
/* ====================================================================== */

/*
 * The shipped Z3660 config (autoconfig_rtg NO) puts the board at the fixed base
 * 0x10000000 -- below VSECT1, inside AMIX's identity-mapped section 0 -- so
 * z3660map() must take the DIRECT arm and call no sptalloc() at all.  The mock's
 * autocon() reports exactly that base, so the arm the harness exercises is the
 * arm the metal box runs.  This gate exists so that stays true: if the threshold,
 * the window span, or the mock base ever drifts such that the driver falls back
 * to sptalloc(), every other test would still pass (both arms reach the same mock
 * buffers) and the regression would be invisible.  Run it first -- the CDB tests
 * below all map the board as a side effect of their first command.
 */
static void test_direct_mapping()
{
	printf("[GATE] board mapped DIRECTLY (no sptalloc below VSECT1)\n");
	CHECK( z3660_direct_map == 0,
	       "z3660_direct_map is 0 before the board is mapped");
	mock_trips_reset();
	CHECK( z3660present( &g_probe_base) == 1, "z3660present() finds the board");
	printf("       base=0x%08lX direct_map=%d\n",
	       (unsigned long)g_probe_base, (int)z3660_direct_map);
	CHECK( z3660_direct_map == 1,
	       "z3660map() took the section-0 identity arm, not sptalloc()");

	/*
	 * The per-unit static-geometry cache must be filled HERE, at attach --
	 * one sweep of all 8 piscsi units -- so that no later CDB pays for it.
	 * Gating the probe's trip breakdown pins that down: eight BLOCKSIZE0+4n
	 * reads, eight BLOCKS0+4n reads and eight PDT reads (each preceded by its
	 * own DRVNUMX select, plus the one DRVNUMX the DRVTYPE presence probe
	 * itself writes).  If the fill were ever moved back into the command path
	 * these counts would drop to zero and the hot-path gate below would rise.
	 */
	printf("       probe trips=%lu (rd=%lu wr=%lu) drvnumx=%lu blocksize=%lu blocks=%lu pdt=%lu\n",
	       mock_trips, mock_trips_rd, mock_trips_wr, mock_trips_drvnumx,
	       mock_trips_blocksize, mock_trips_blocks, mock_trips_pdt);
	CHECK( mock_trips_blocksize == 8,
	       "attach fills BLOCKSIZE for all 8 units (geometry cached at probe)");
	CHECK( mock_trips_blocks == 8,
	       "attach fills BLOCKS for all 8 units (geometry cached at probe)");
	CHECK( mock_trips_pdt == 8,
	       "attach fills PDT for all 8 units (geometry cached at probe)");
	CHECK( mock_trips_drvnumx == 9,
	       "attach selects each unit before its PDT read (8) + the DRVTYPE probe (1)");
}

/* ====================================================================== */
/* GATING: per-CDB round-trip budget.                                      */
/*                                                                         */
/* Every mailbox register access is one cross-core round trip during which  */
/* core1 -- the guest's own CPU -- hard-spins retiring no instructions,     */
/* while core0 services it from a cooperative protothread loop that handles */
/* at most ONE access per iteration (Z3660 docs/piscsi-service-path.md      */
/* sections 1.1-1.3).  Round trips, not bytes, are what a 2 KB request      */
/* costs, so the trip budget per CDB is the driver's real performance       */
/* contract and belongs in a gate.                                          */
/*                                                                         */
/* That doc's table 1.1 measured the pre-cache driver at TEN trips per 2 KB */
/* READ, of which nine carried no payload:                                  */
/*                                                                         */
/*   1  WRLONG DRVNUMX        select the unit          <- kept              */
/*   2  RDLONG BLOCKSIZE0+4n  static after attach      <- cached            */
/*   3  WRLONG DRVNUMX        redundant repeat of 1    <- deleted           */
/*   4  RDLONG PDT            static after attach      <- cached            */
/*   5  RDLONG BLOCKS0+4n     static after attach      <- cached            */
/*   6  WRLONG READ_ADDR1     block number                                  */
/*   7  WRLONG READ_ADDR2     byte length                                   */
/*   8  WRLONG READ_ADDR3     buffer address                                */
/*   9  WRLONG READ           the doorbell (carries the data)               */
/*  10  RDLONG USED_DMA       bounce-or-direct verdict                      */
/*                                                                         */
/* Trips 2-5 are now served from the per-unit cache filled at attach, so a  */
/* steady-state READ costs SIX and a WRITE (no USED_DMA readback) FIVE.     */
/* The gate asserts the exact totals and, separately, that the three static */
/* geometry registers are not touched at all on the command path -- so a    */
/* regression that reinstated one of them fails loudly and by name.         */
/* ====================================================================== */

static void test_hotpath_trip_budget()
{
	uchar		cdb[10], buf[2048], src[512];
	unsigned char	*back = mock_backing( U_DISK);
	int		i;
	unsigned long	rd_trips, wr_trips;

	printf("\n=== GATE: per-CDB mailbox round-trip budget ===\n");

	/* ---- steady-state READ(10): 4 x 512-byte blocks @ LBA 8 (one chunk) ---- */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[5] = 8; cdb[8] = 4;
	memset( buf, 0, sizeof buf);
	mock_trips_reset();
	run_cmd( U_DISK, cdb, 10, buf, 2048);
	rd_trips = mock_trips;
	printf("       READ(10) 2KB: trips=%lu (rd=%lu wr=%lu) drvnumx=%lu blocksize=%lu blocks=%lu pdt=%lu\n",
	       mock_trips, mock_trips_rd, mock_trips_wr, mock_trips_drvnumx,
	       mock_trips_blocksize, mock_trips_blocks, mock_trips_pdt);
	CHECK( g_okay && g_status == 0, "trip budget: READ(10) still completes GOOD");
	CHECK( memcmp( buf, back + 8 * 512, 2048) == 0,
	       "trip budget: READ(10) data still correct (cached bs scales the LBA)");
	CHECK( mock_trips_blocksize == 0,
	       "trip budget: READ(10) reads NO BLOCKSIZE register (served from cache)");
	CHECK( mock_trips_blocks == 0,
	       "trip budget: READ(10) reads NO BLOCKS register (served from cache)");
	CHECK( mock_trips_pdt == 0,
	       "trip budget: READ(10) reads NO PDT register (served from cache)");
	CHECK( mock_trips_drvnumx == 1,
	       "trip budget: READ(10) writes DRVNUMX exactly once (redundant repeat gone)");
	CHECK( rd_trips == 6,
	       "trip budget: READ(10) costs 6 round trips (was 10 before the cache)");

	/* ---- steady-state WRITE(10): 1 x 512-byte block @ LBA 12 ---- */
	for (i = 0; i < 512; i++) src[i] = (uchar)(0x30 + (i & 0x0F));
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x2A; cdb[5] = 12; cdb[8] = 1;
	mock_trips_reset();
	run_cmd( U_DISK, cdb, 10, src, 512);
	wr_trips = mock_trips;
	printf("       WRITE(10) 512B: trips=%lu (rd=%lu wr=%lu) drvnumx=%lu blocksize=%lu blocks=%lu pdt=%lu\n",
	       mock_trips, mock_trips_rd, mock_trips_wr, mock_trips_drvnumx,
	       mock_trips_blocksize, mock_trips_blocks, mock_trips_pdt);
	CHECK( g_okay && g_status == 0, "trip budget: WRITE(10) still completes GOOD");
	CHECK( memcmp( back + 12 * 512, src, 512) == 0,
	       "trip budget: WRITE(10) still lands in backing[12*512..]");
	CHECK( mock_trips_blocksize == 0 && mock_trips_blocks == 0 && mock_trips_pdt == 0,
	       "trip budget: WRITE(10) touches none of the three static geometry registers");
	CHECK( mock_trips_drvnumx == 1,
	       "trip budget: WRITE(10) writes DRVNUMX exactly once");
	CHECK( wr_trips == 5,
	       "trip budget: WRITE(10) costs 5 round trips (was 9 before the cache)");

	/*
	 * The cached facts must still be the RIGHT facts -- a cache that answers
	 * cheaply but wrongly would pass every count above.  READ CAPACITY(10)
	 * reports block size and block count straight out of the cache; assert
	 * both are the mock's geometry AND that answering cost zero wire reads.
	 */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x25;
	memset( buf, 0xEE, sizeof buf);
	mock_trips_reset();
	run_cmd( U_DISK, cdb, 10, buf, 8);
	printf("       READ CAPACITY from cache: trips=%lu last_lba=0x%08lX blklen=%lu\n",
	       mock_trips, be32( buf), be32( buf + 4));
	CHECK( be32( buf) == DK_NBLK - 1 && be32( buf + 4) == DK_BS,
	       "trip budget: cached geometry is CORRECT (last LBA + block length)");
	CHECK( mock_trips == 1,
	       "trip budget: READ CAPACITY costs 1 trip (the DRVNUMX select) -- geometry cached");
}

/*
 * GATING: an UNMAPPED unit must never be cached as absent.
 *
 * "Is a drive mapped at this unit at all" is the one fact a firmware rescan can
 * legitimately change, so the driver marks a cache entry valid only when the unit
 * reports a nonzero block count.  Two things must therefore hold for a unit with
 * no drive behind it:
 *
 *   1. The I/O is still REFUSED.  The firmware silently no-ops a read or write to
 *      an unmapped drive, so reporting GOOD would hand the caller a buffer full of
 *      whatever was already there -- which is why the driver has always treated
 *      nblocks == 0 as a hard error.  A cache must not weaken that.
 *   2. The unit is RE-READ on the next command rather than being permanently
 *      poisoned by its first (empty) probe.  Asserting the trip breakdown twice
 *      proves the re-read: identical non-zero geometry-register counts both times.
 *
 * This is the one path where the cache deliberately does NOT save trips; it costs
 * one extra DRVNUMX write versus the pre-cache driver, on a command that is
 * refused anyway, and only ever during sd.c's boot-time bus scan.
 */
#define U_EMPTY	3		/* no mock_add_drive() for this unit */

static void test_unmapped_unit_not_cached()
{
	uchar		cdb[10], buf[512];
	unsigned long	first_trips;

	printf("\n=== GATE: unmapped unit refused, and never cached as absent ===\n");

	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[8] = 1;	/* READ(10) 1 block */
	memset( buf, 0xEE, sizeof buf);
	mock_trips_reset();
	run_cmd( U_EMPTY, cdb, 10, buf, 512);
	first_trips = mock_trips;
	printf("       pass 1: trips=%lu blocksize=%lu blocks=%lu pdt=%lu status=0x%02X okay=%d\n",
	       mock_trips, mock_trips_blocksize, mock_trips_blocks, mock_trips_pdt,
	       g_status, (int)g_okay);
	CHECK( !g_okay && g_status == 0xff,
	       "unmapped unit: READ(10) REFUSED (firmware would silently no-op it)");
	CHECK( mock_trips_blocks == 1,
	       "unmapped unit: geometry was actually probed (cache miss, not a stale hit)");

	/* Second identical command: the miss must repeat, i.e. nothing was latched. */
	memset( buf, 0xEE, sizeof buf);
	mock_trips_reset();
	run_cmd( U_EMPTY, cdb, 10, buf, 512);
	printf("       pass 2: trips=%lu blocksize=%lu blocks=%lu pdt=%lu status=0x%02X okay=%d\n",
	       mock_trips, mock_trips_blocksize, mock_trips_blocks, mock_trips_pdt,
	       g_status, (int)g_okay);
	CHECK( !g_okay && g_status == 0xff,
	       "unmapped unit: second READ(10) still refused");
	CHECK( mock_trips_blocks == 1 && mock_trips_blocksize == 1 && mock_trips_pdt == 1,
	       "unmapped unit: RE-PROBED on every command (absence never cached)");
	CHECK( mock_trips == first_trips,
	       "unmapped unit: identical trip cost both passes (no latched state)");

	/*
	 * And the mapped units must be untouched by all of that -- a fill for one
	 * unit must not disturb another's cached entry.
	 */
	memset( cdb, 0, sizeof cdb); cdb[0] = 0x25;		/* READ CAPACITY(10) */
	memset( buf, 0xEE, sizeof buf);
	mock_trips_reset();
	run_cmd( U_DISK, cdb, 10, buf, 8);
	CHECK( g_okay && be32( buf) == DK_NBLK - 1 && be32( buf + 4) == DK_BS,
	       "unmapped unit: mapped unit's cached geometry still intact afterwards");
	CHECK( mock_trips == 1,
	       "unmapped unit: mapped unit still answers from cache (1 trip)");
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
/* GATING: spl6 re-entry bracket -- a callout-nested mailbox transaction    */
/* cannot interleave an in-flight one (the T2.P3 corruption family).        */
/* ====================================================================== */

static int reentry_hook_calls;	/* times the in-flight hook fired          */
static int reentry_reentered;	/* did a nested z3660queue() actually run?  */
static int reentry_guard;	/* re-enter at most once (no infinite loop) */

/*
 * The simulated clock-callout path: while a mailbox transaction is in flight,
 * a level-2 clock tick would dispatch z3660done -> ihandle -> startio -> a
 * SECOND z3660queue().  The mock fires this hook from inside the in-flight
 * command (do_read/do_write).  It only actually re-enters when the clock is
 * UNMASKED -- exactly the gate spl6() closes on real hardware.  When the driver
 * holds spl6 (bracket present) mock_clock_masked() is true and the callout is
 * deferred, so nothing interleaves; with the bracket artificially absent the
 * nested transaction runs mid-flight and the driver's z3660_nest_hits records
 * the breach.
 */
static void reentry_hook()
{
	static uchar	nbuf[2048];
	struct sdcom	sc;

	reentry_hook_calls++;
	if (reentry_guard)
		return;
	reentry_guard = 1;
	if (mock_clock_masked())
		return;			/* spl6 bracket holds -> callout deferred */

	reentry_reentered = 1;
	memset( &sc, 0, sizeof sc);
	sc.unit  = (uint)U_CD;
	sc.addr  = (caddr_t)nbuf;
	sc.nbyte = 2048;
	sc.cdb[0] = 0x28; sc.cdb[5] = 5; sc.cdb[8] = 1;	/* READ(10) 1 sector @ LBA 5 */
	sc.intr  = (void (*)())test_intr;
	z3660queue( U_CD, &sc);		/* nested transaction, mid-flight */
}

static void test_reentry()
{
	uchar		cdb[10], buf[2048];
	unsigned char	*back = mock_backing( U_CD);

	printf("\n=== GATE: spl6 bracket vs callout-nested mailbox transaction ===\n");

	/* ---- Run 1: bracket PRESENT -- spl6 raises IPL, clock masked ---- */
	z3660_nest_depth = 0; z3660_nest_hits = 0;
	reentry_hook_calls = reentry_reentered = reentry_guard = 0;
	mock_spl_disabled = 0;
	mock_set_inflight_hook( reentry_hook);

	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[8] = 1;	/* READ(10) LBA 0 */
	memset( buf, 0, sizeof buf);
	run_cmd( U_CD, cdb, 10, buf, 2048);

	CHECK( reentry_hook_calls >= 1, "re-entry: in-flight hook fired");
	CHECK( reentry_reentered == 0, "re-entry: nested transaction DEFERRED by spl6 bracket");
	CHECK( z3660_nest_hits == 0, "re-entry: no nesting hit while bracketed");
	CHECK( g_okay && g_status == 0, "re-entry: outer READ still completes GOOD");
	CHECK( memcmp( buf, back + 0, 2048) == 0,
	       "re-entry: outer READ data intact (no interleave)");

	/* ---- Run 2: bracket ABSENT -- spl6 no-op, clock unmasked ---- */
	z3660_nest_depth = 0; z3660_nest_hits = 0;
	reentry_hook_calls = reentry_reentered = reentry_guard = 0;
	mock_spl_disabled = 1;			/* artificially remove the bracket */
	mock_set_inflight_hook( reentry_hook);

	memset( cdb, 0, sizeof cdb); cdb[0] = 0x28; cdb[8] = 1;	/* READ(10) LBA 0 */
	memset( buf, 0, sizeof buf);
	run_cmd( U_CD, cdb, 10, buf, 2048);

	CHECK( reentry_reentered == 1, "re-entry: nested transaction interleaves when bracket absent");
	CHECK( z3660_nest_hits >= 1, "re-entry: z3660_nest_hits counts the attempted re-entry");
	printf("       (bracket-absent nest_hits=%lu, outer READ %s)\n",
	       z3660_nest_hits,
	       memcmp( buf, back + 0, 2048) == 0 ? "clean" : "CORRUPTED by interleave");

	/* restore harness state for anything that runs after */
	mock_set_inflight_hook( (void (*)())0);
	mock_spl_disabled = 0;
	z3660_nest_depth = 0; z3660_nest_hits = 0;
}

/* ====================================================================== */
/* GATING: synchronous ITERATIVE completion (the T2.P3 lifetime fix).       */
/* A completion that re-issues the next I/O -- exactly dd.c's ihandle ->     */
/* startio -> sdqueue -> z3660queue chain -- must (1) be delivered while the */
/* caller's context is current (no timeout() deferral window that strands a  */
/* stack sdcom) and (2) NOT recurse one kernel-stack frame per I/O.  This    */
/* drives a long re-issue chain through a PERSISTENT sdcom (like &dp->com)   */
/* and asserts every completion lands at a fixed, small nesting depth.       */
/* ====================================================================== */

extern unsigned long z3660_cq_overflow;	/* driver: completion-FIFO overrun count */

static void		tramp_reissue();
static struct sdcom	tramp_sc;	/* persistent -- mimics dd.c's &dp->com   */
static int		tramp_depth, tramp_max_depth, tramp_deliveries, tramp_chain;

static void tramp_intr( cp)
struct sdcom	*cp;
{
	tramp_depth++;
	if (tramp_depth > tramp_max_depth)
		tramp_max_depth = tramp_depth;
	tramp_deliveries++;
	if (tramp_chain > 0) {			/* re-issue the next op, like ihandle */
		tramp_chain--;
		tramp_reissue( (int)cp->unit);
	}
	tramp_depth--;
}

static void tramp_reissue( unit)
int	unit;
{
	memset( &tramp_sc, 0, sizeof tramp_sc);
	tramp_sc.unit   = (uint)unit;
	tramp_sc.cdb[0] = 0x00;			/* TEST UNIT READY: no data, always GOOD */
	tramp_sc.intr   = (void (*)())tramp_intr;
	z3660queue( unit, &tramp_sc);
}

static void test_completion_trampoline()
{
	printf("\n=== GATE: synchronous iterative completion (no per-I/O recursion) ===\n");

	z3660_nest_depth = z3660_nest_hits = 0;
	z3660_cq_overflow = 0;
	tramp_depth = tramp_max_depth = tramp_deliveries = 0;
	tramp_chain = 50;			/* initial completion + 50 re-issues = 51 */

	tramp_reissue( U_DISK);			/* kick off the re-issue chain */

	printf("       deliveries=%d max_depth=%d nest_hits=%lu cq_overflow=%lu\n",
	       tramp_deliveries, tramp_max_depth, z3660_nest_hits, z3660_cq_overflow);
	CHECK( tramp_deliveries == 51,
	       "trampoline: all 51 chained completions delivered (none stranded)");
	CHECK( tramp_max_depth <= 2,
	       "trampoline: completion depth bounded (iterative, not per-I/O recursion)");
	CHECK( z3660_cq_overflow == 0, "trampoline: completion FIFO never overran");
	CHECK( z3660_nest_hits == 0, "trampoline: no mailbox re-entry across the burst");

	z3660_nest_depth = z3660_nest_hits = 0;
}

/* ====================================================================== */
/* ------------------------------------------------------------------------
 * BLIZZARD F3 -- cache-maintenance boundaries (docs/BLIZZARD-F3.md)
 *
 * What this CAN prove on a host: that the gate really gates, that the ops land
 * at the right boundaries and only there, that the READ bounce arm does NOT
 * invalidate (the trap that would destroy data), and that the free-ride
 * assertion fails when the free ride is taken away.
 *
 * What it CANNOT prove: anything about a real cache.  There isn't one here, and
 * Amiberry has none either -- only the 68060 can score correctness, by the
 * canary protocol pre-registered in docs/BLIZZARD-F3.md section 7.
 *
 * Note on coverage: the mock sets USED_DMA to the buffer address, so every
 * harness READ takes the BOUNCE arm.  The direct-DMA arm's invalidate is
 * therefore unreachable here by construction, and is covered by disassembly
 * plus the metal protocol instead.
 * ------------------------------------------------------------------------ */
extern long		z3660_cache, z3660_ci_enforce;
extern unsigned long	z3660_push_n, z3660_inv_n;
extern unsigned long	z3660_push_bytes, z3660_inv_bytes;
extern unsigned long	z3660_bounce_wr_n, z3660_bounce_rd_n;
extern unsigned long	z3660_pagecross_n, z3660_range_ovf;
extern unsigned long	z3660_ci_ok, z3660_sptalloc_unsafe;
extern unsigned long	z3660_test_dtt0, z3660_test_dtt1;
extern int		z3660_ci_check();

/*
 * The harness .c files deliberately do NOT get -Istubs (they see the real system
 * headers), so the page size is restated here.  It must match NBPP in
 * stubs/sys/immu.h; if it stops matching, the page-cross gate below fails loudly
 * rather than silently measuring nothing.
 */
#define	F3_NBPP		2048

static uchar	f3buf[16384];

static void f3_zero()
{
	z3660_push_n = z3660_inv_n = z3660_push_bytes = z3660_inv_bytes = 0;
	z3660_bounce_wr_n = z3660_bounce_rd_n = 0;
	z3660_pagecross_n = z3660_range_ovf = 0;
}

/* one INQUIRY / READ(10) / WRITE(10) triple against U_DISK, into `p` */
static void f3_triple( p)
uchar	*p;
{
	uchar	cdb[10];

	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x12; cdb[4] = 36;			/* INQUIRY 36 */
	run_cmd( U_DISK, cdb, 6, p, 36);
	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x28; cdb[8] = 1;			/* READ(10) 1 block */
	run_cmd( U_DISK, cdb, 10, p, (unsigned)DK_BS);
	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x2A; cdb[8] = 1;			/* WRITE(10) 1 block */
	run_cmd( U_DISK, cdb, 10, p, (unsigned)DK_BS);
}

static void test_f3_cache()
{
	uchar		*p, *cross;
	uchar		cdb[10];
	unsigned long	save0, save1;
	int		saved_map;

	printf("\n=== GATE: F3 cache maintenance -- gate, boundaries, bounce arm ===\n");

	/* ---- 1. the shipping default: no line op is reachable at all ---- */
	z3660_cache = 0;
	f3_zero();
	f3_triple( f3buf);
	CHECK( z3660_push_n == 0 && z3660_inv_n == 0,
	       "z3660_cache=0: no cache line op on any path (the shipping default)");
	printf("       cache=0: push_n=%lu inv_n=%lu bounce_rd=%lu\n",
	       z3660_push_n, z3660_inv_n, z3660_bounce_rd_n);

	/* ---- 2. armed: the ops land at the boundaries, and only there ---- */
	z3660_cache = 60;

	f3_zero();					/* INQUIRY alone */
	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x12; cdb[4] = 36;
	run_cmd( U_DISK, cdb, 6, f3buf, 36);
	CHECK( z3660_push_n == 1 && z3660_push_bytes == 36,
	       "synthesised INQUIRY: exactly one push, over exactly nbyte");
	CHECK( z3660_inv_n == 0,
	       "synthesised INQUIRY: never invalidated (would discard CPU writes)");

	f3_zero();					/* READ(10), one chunk */
	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x28; cdb[8] = 1;
	run_cmd( U_DISK, cdb, 10, f3buf, (unsigned)DK_BS);
	CHECK( z3660_push_n == 1 && z3660_push_bytes == DK_BS,
	       "READ(10): FROM_DEVICE prepare pushes the chunk before the doorbell");
	CHECK( z3660_bounce_rd_n == 1,
	       "READ(10): the mock returns USED_DMA, so the bounce arm ran");
	CHECK( z3660_inv_n == 0,
	       "READ(10) BOUNCE arm: NO invalidate -- it would discard the bcopy");

	f3_zero();					/* WRITE(10), one chunk */
	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x2A; cdb[8] = 1;
	run_cmd( U_DISK, cdb, 10, f3buf, (unsigned)DK_BS);
	CHECK( z3660_push_n == 1 && z3660_push_bytes == DK_BS,
	       "WRITE(10): TO_DEVICE prepare pushes the chunk before the doorbell");
	CHECK( z3660_inv_n == 0,
	       "WRITE(10): TO_DEVICE never invalidates at completion");
	CHECK( z3660_bounce_wr_n == 0,
	       "WRITE(10): the driver-side staging branch stays unreachable");

	/* ---- 3. multi-chunk: one prepare per chunk, not per command ---- */
	f3_zero();
	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x28; cdb[8] = 4;			/* 4 blocks, still one chunk */
	run_cmd( U_DISK, cdb, 10, f3buf, (unsigned)(4 * DK_BS));
	CHECK( z3660_push_n == 1 && z3660_push_bytes == 4 * DK_BS,
	       "READ(10) x4 blocks: one prepare covering the whole chunk");

	/* ---- 4. the S11 page-cross census counts, and does NOT refuse ---- */
	f3_zero();
	p = f3buf;					/* land 64 bytes below a page end */
	cross = p + ((F3_NBPP - ((unsigned long)p & (F3_NBPP - 1))) & (F3_NBPP - 1));
	cross = cross - 64;
	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x28; cdb[8] = 1;			/* 512B from 64B before a page end */
	run_cmd( U_DISK, cdb, 10, cross, (unsigned)DK_BS);
	CHECK( z3660_pagecross_n == 1,
	       "S11: a chunk spanning a page boundary is counted");
	CHECK( g_okay == TRUE,
	       "S11: and is NOT refused this round (census only -- see BLIZZARD-F3 3.4)");

	f3_zero();					/* page-aligned: no count */
	p = f3buf + ((F3_NBPP - ((unsigned long)f3buf & (F3_NBPP - 1))) & (F3_NBPP - 1));
	memset( cdb, 0, sizeof cdb);
	cdb[0] = 0x28; cdb[8] = 1;
	run_cmd( U_DISK, cdb, 10, p, (unsigned)DK_BS);
	CHECK( z3660_pagecross_n == 0,
	       "S11: an in-page chunk is not counted");

	/* ---- 5. the census is UNGATED: it must run on the shipping boxes ---- */
	z3660_cache = 0;
	f3_zero();
	run_cmd( U_DISK, cdb, 10, cross, (unsigned)DK_BS);
	CHECK( z3660_pagecross_n == 1 && z3660_push_n == 0,
	       "S11 census runs with the cache gate OFF (that is where it is read)");

	/* ---- 6. F3-M0: the free-ride assertion, both arms ---- */
	save0 = z3660_test_dtt0;
	save1 = z3660_test_dtt1;
	z3660_cache      = 60;
	z3660_ci_enforce = 0;

	z3660_ci_ok = 0;
	CHECK( z3660_ci_check( 0x10000000UL, 0x10090000UL) == 0 && z3660_ci_ok == 1,
	       "M0: shipped DTT0 0x003fc060 passes the free-ride check");

	z3660_test_dtt0 = 0x003fc000;		/* CM=00: cacheable write-through */
	z3660_ci_ok = 0;
	CHECK( z3660_ci_check( 0x10000000UL, 0x10090000UL) == 0 && z3660_ci_ok == 2,
	       "M0: a CACHEABLE covering TTR fails the check");

	z3660_test_dtt0 = 0x003f4060;		/* E=0: covers nothing */
	z3660_ci_ok = 0;
	CHECK( z3660_ci_check( 0x10000000UL, 0x10090000UL) == 0 && z3660_ci_ok == 2,
	       "M0: an address no enabled TTR covers fails the check");

	z3660_test_dtt0  = 0x003fc060;		/* good again, but enforce refuses */
	z3660_ci_enforce = 1;
	z3660_ci_ok = 0;
	saved_map = (int)z3660_direct_map;
	z3660_direct_map = 0;			/* pretend the sptalloc arm was taken */
	CHECK( z3660_ci_check( 0x10000000UL, 0x10090000UL) != 0 && z3660_ci_ok == 2
	       && z3660_sptalloc_unsafe == 1,
	       "M0: the sptalloc arm is refused when enforcement is on");
	z3660_direct_map = (unsigned char)saved_map;

	/* ---- restore the shipping posture for anything that follows ---- */
	z3660_test_dtt0  = save0;
	z3660_test_dtt1  = save1;
	z3660_cache      = 0;
	z3660_ci_enforce = 0;
	z3660_sptalloc_unsafe = 0;
	f3_zero();
	printf("       restored: cache=%ld ci_enforce=%ld\n",
	       z3660_cache, z3660_ci_enforce);
}

int main()
{
	mock_reset();
	mock_add_drive( U_CD,   0x05, CD_BS, CD_NBLK, 128UL * CD_BS);   /* 256 KB backing */
	mock_add_drive( U_DISK, 0x00, DK_BS, DK_NBLK, 256UL * DK_BS);   /* 128 KB backing */

	printf("=== Z3660 SCSI host CDB test (real src/z3660.c, -DHOST_TEST) ===\n\n");

	test_direct_mapping();

	test_cd_inquiry();
	test_cd_read_capacity();
	test_cd_read10();
	test_cd_write_rejected();

	test_disk_inquiry_frozen();
	test_disk_read_capacity_frozen();
	test_disk_read10_frozen();
	test_disk_write10_frozen();

	test_tur_both();

	test_hotpath_trip_budget();
	test_unmapped_unit_not_cached();

	test_reentry();
	test_completion_trampoline();

	test_f3_cache();

	stretch_cd_parity();

	printf("\n=== GATING: %d passed, %d failed ===\n", g_pass, g_fail);
	printf("=== STRETCH parity: %d match, %d deviation ===\n", s_match, s_dev);
	return g_fail ? 1 : 0;
}
