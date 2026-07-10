#ifndef _HOSTTEST_SYS_TYPES_H
#define _HOSTTEST_SYS_TYPES_H
/*
 * Minimal SVR4 <sys/types.h> shim for the host harness.  The real Amix header
 * typedefs caddr_t plus the uint/ushort/ulong family; z3660.c only needs
 * caddr_t from here -- rico.h macro-defines the u* short names immediately
 * afterwards, and nothing in the driver uses them before that.
 *
 * Shadows the system header ONLY inside the z3660.c compile (that TU is built
 * with -Istubs and no system includes); the harness .c files do not use -Istubs
 * and see the real <sys/types.h>.
 */
typedef char *caddr_t;
#endif
