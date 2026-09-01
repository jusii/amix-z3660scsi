#ifndef Z3660_MOCK_REGS_H
#define Z3660_MOCK_REGS_H
/*
 * HOST_TEST register seam.  Force-included (-include) into the z3660.c compile
 * so the driver's own "#ifndef HOST_TEST" WRLONG/RDLONG definitions are skipped
 * and these take their place, routing every mailbox access into the mock piscsi
 * firmware model in mock_piscsi.c instead of a real volatile MMIO deref.
 *
 * Register cells are unsigned long (64-bit on LP64 Linux) so that a host buffer
 * POINTER handed to READ_ADDR3 / WRITE_ADDR3 survives intact.  On real hardware
 * that slot carries a 32-bit Amiga physical address that the CALLER produced with
 * vtop() before filling cp->addr -- the driver stores cp->addr straight into the
 * register and never translates it (NOTES.md 2026-07-11 (a): a vtop() inside
 * z3660.c would double-translate).  Under HOST_TEST that same contract makes
 * vtop() effectively the identity (the test stores the raw host buffer pointer in
 * cp->addr) and we pass the pointer through unmapped.
 */
extern unsigned long z3660_mock_rdlong(unsigned int cmd);
extern void          z3660_mock_wrlong(unsigned int cmd, unsigned long val);

#define WRLONG(cmd,val)	z3660_mock_wrlong((unsigned int)(cmd), (unsigned long)(val))
#define RDLONG(cmd)	z3660_mock_rdlong((unsigned int)(cmd))
#endif
