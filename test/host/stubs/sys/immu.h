#ifndef _HOSTTEST_SYS_IMMU_H
#define _HOSTTEST_SYS_IMMU_H
/*
 * Minimal <sys/immu.h> shim: the paging names z3660map() references.  The
 * harness stubs sptalloc() to hand back mock buffers and ignore the frame
 * number / flags, so phystopfn() and PG_V only have to exist and be inert.
 */
typedef unsigned long paddr_t;
#define PG_V 1
#define phystopfn(x) ((unsigned long)(x))
#endif
