#ifndef _HOSTTEST_SYS_INLINE_H
#define _HOSTTEST_SYS_INLINE_H
/*
 * Host-harness stub for the amiga <sys/inline.h>.  The real header emits the
 * m68k spl instructions inline (spl6 => _spl4 => `move.w #0x2400,%sr`, IPL 4;
 * splx restores the SR word).  On the host there is no SR, so the driver's
 * spl6()/splx() mailbox-transaction bracket resolves to the MOCK spl in
 * kstubs.c instead -- which tracks a mock IPL level plus call counters so the
 * re-entry test can run the driver both WITH the bracket (IPL raised, the
 * simulated clock callout masked) and with it artificially absent.
 *
 * Only the two symbols the driver actually uses are declared here.  This file
 * is seen ONLY by the z3660.c compile (built with -Istubs); the harness .c
 * translation units do not use -Istubs and never include it.
 */
extern int spl6();
extern int splx();
#endif
