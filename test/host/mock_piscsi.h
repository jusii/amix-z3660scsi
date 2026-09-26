#ifndef Z3660_MOCK_PISCSI_H
#define Z3660_MOCK_PISCSI_H
/*
 * Test-facing control of the mock Z3660 piscsi mailbox (mock_piscsi.c).
 *
 * The mailbox is a per-drive devs[] table kept in LOCKSTEP with the FIRMWARE
 * register semantics in
 *   Z3660/z3660-firmware/.../Z3660/src/scsi/scsi.c
 *     handle_piscsi_reg_write()  (~line 1043)
 *     handle_piscsi_reg_read()   (~line 1600)
 *   Z3660/.../Z3660/src/scsi/z3660_scsi_enums.h  (register offsets)
 * and mirrored by the emulator target Z3660_emu/src/uae/a3000_scsi.cpp.
 * See mock_piscsi.c for the register-by-register contract.
 */

/* Reset all mock state; allocate the shared DMA bounce buffer. Call first. */
void  mock_reset(void);

/*
 * Attach a drive at `unit` with the given peripheral device type (0x00 disk,
 * 0x05 CD-ROM), block size and total block count.  A backing store of
 * backing_len bytes is allocated and filled with a deterministic,
 * position-dependent pattern; the first four bytes of every whole block hold
 * that block's index big-endian (a marker the read tests check to prove
 * block-index scaling rather than byte addressing).
 */
void  mock_add_drive(int unit, unsigned long pdt,
                     unsigned long block_size, unsigned long nblocks,
                     unsigned long backing_len);

unsigned char *mock_backing(int unit);      /* backing store pointer, or 0 */
unsigned long  mock_backing_len(int unit);  /* backing store length         */

/*
 * Mock spl / re-entry harness (spl impl in kstubs.c, in-flight hook in
 * mock_piscsi.c).  Models the driver's spl6()/splx() mailbox-transaction
 * bracket and the simulated clock callout that tries to nest a second
 * transaction inside an in-flight one.
 */
extern int  mock_spl_disabled;              /* set => spl6() is a no-op (bracket absent) */
extern long mock_spl6_calls, mock_splx_calls;
int  mock_clock_masked(void);               /* nonzero => level-2 clock masked (IPL>=4) */
void mock_spl_reset(void);                  /* reset mock IPL + spl counters/knob        */

/*
 * Register an in-flight hook the mailbox fires from inside a READ/WRITE command
 * (the synchronous "ARM is busy" window) -- the seam the re-entry test uses to
 * simulate a clock tick landing mid-transaction.  Pass 0 to clear.  Reset by
 * mock_reset().
 */
void mock_set_inflight_hook(void (*fn)());

/*
 * Round-trip accounting.  On real hardware EVERY register access is one
 * cross-core handshake: core1 (the guest's CPU) publishes the access into the
 * SHARED struct and then HARD-SPINS, retiring no guest instructions, until
 * core0's cooperative protothread loop happens to service it
 * (Z3660 docs/piscsi-service-path.md sections 1.1-1.3).  So counting register
 * accesses here counts exactly the thing that costs time on metal, and a
 * per-CDB trip budget is a meaningful, mock-independent regression gate.
 *
 * Counters are cumulative; mock_trips_reset() zeroes them (mock_reset() does
 * too).  The category counters break out the four accesses the per-unit static
 * geometry cache in src/z3660.c is there to eliminate from the hot path.
 */
extern unsigned long mock_trips;            /* ALL register accesses (rd + wr)   */
extern unsigned long mock_trips_rd;         /* RDLONG only                       */
extern unsigned long mock_trips_wr;         /* WRLONG only                       */
extern unsigned long mock_trips_drvnumx;    /* writes to DRVNUMX  (0x90)         */
extern unsigned long mock_trips_blocksize;  /* reads of BLOCKSIZE0+4n (0x200+)   */
extern unsigned long mock_trips_blocks;     /* reads of BLOCKS0+4n    (0x220+)   */
extern unsigned long mock_trips_pdt;        /* reads of PDT       (0xA0)         */
void mock_trips_reset(void);

/*
 * Stock-firmware hazard accounting (see mock_piscsi.c).  mock_div0 counts reads
 * of BLOCKS0+4n for a unit whose block_size is 0 -- the read on which stock
 * firmware divides by zero; mock_doorbells[u] counts READ/WRITE triggers for
 * unit u.  Both are zeroed only by mock_reset().
 */
extern unsigned long mock_div0;
extern unsigned long mock_doorbells[8];
void mock_set_geom(int unit, unsigned long pdt,
                   unsigned long block_size, unsigned long nblocks);

#endif
