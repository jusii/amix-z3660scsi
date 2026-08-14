#ifndef _HOSTTEST_SYS_IMMU_H
#define _HOSTTEST_SYS_IMMU_H
/*
 * Minimal <sys/immu.h> shim: the paging names z3660map() references.
 *
 * VSECT1 and NBPP carry their REAL kernel values (kernel immu.h:288 and :54) so
 * the driver takes the same arm on the host that it takes on metal: the mock's
 * autocon() reports the shipped fixed base 0x10000000, which is below VSECT1,
 * so z3660map() maps DIRECTLY and never calls sptalloc().
 *
 * phystokv() cannot be the identity here.  On AMIX it is (kernel immu.h:348)
 * because the supervisor root table identity-maps the whole low 1 GB; the host
 * has no such map and dereferencing 0x10002000 would fault.  So the harness
 * routes it into the mock, which hands back the very same g_regs / g_bounce
 * buffers its sptalloc() stub returns.  Both mapping arms therefore land on the
 * mock's buffers, and the driver source stays byte-identical between the kernel
 * build and this one.
 *
 * sptalloc() -- still reached by the >= VSECT1 arm -- ignores the frame number
 * and the flags, so phystopfn() and PG_V only have to exist and be inert.
 */
typedef unsigned long paddr_t;
#define PG_V 1
#define phystopfn(x) ((unsigned long)(x))

#define NBPP   2048		/* kernel immu.h:54  -- Amix pages are 2KB */
#define VSECT1 0x40000000	/* kernel immu.h:288 -- first table-backed section */

extern unsigned long z3660_mock_phystokv();
#define phystokv(p) z3660_mock_phystokv((unsigned long)(p))
#endif
