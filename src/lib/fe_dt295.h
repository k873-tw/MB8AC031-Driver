/* fe_dt295.h -- PIX-DT295 front end (terrestrial and satellite), tuned by
 * the host.
 *
 * SPDX-License-Identifier: MIT
 *
 * Every write here was kept because taking it out, or flipping one of its
 * bits, changed something on a real PIX-DT295 with CFW (lock, C/N or the
 * TS).  fe_dt295.c says, write by write, what was seen;
 * docs/tuning-dt295.md has the same as tables.
 */
#ifndef MB8_FE_DT295_H
#define MB8_FE_DT295_H

#include <stdint.h>

#include "fe.h"

#define DT295_DEMOD_T   0x10u   /* 7-bit: the terrestrial demodulator     */
#define DT295_DEMOD_S   0x11u   /* 7-bit: the satellite demodulator       */
#define DT295_KHZ_MIN   470000u
#define DT295_KHZ_MAX   700000u /* above it: not measured, refused        */
/* Satellite: the IF from the dish (kHz).  Measured from BS 1 (1,049,480)
 * to CS 24 (2,053,000); outside that, refused. */
#define DT295_SAT_KHZ_MIN 1049000u
#define DT295_SAT_KHZ_MAX 2054000u

/* Once after the device comes up.  0 ok, -1 the bus failed. */
int dt295_init(fe_xfer_fn x, void *ctx);

/* Tune to `khz`.  0 ok, -1 the bus failed. */
int dt295_tune(fe_xfer_fn x, void *ctx, unsigned khz);

/* The tuner's frequency word for `khz` (exposed for the tests). */
unsigned dt295_freq_word(unsigned khz);

/* Lock (1/0) and the raw C/N (demod 0x8B..0x8D; smaller is better).
 * 0 ok, -1 the bus failed. */
int dt295_status(fe_xfer_fn x, void *ctx, int *locked, uint32_t *cnr);

/* --- satellite (ISDB-S) ---------------------------------------------- */

/* Tune the satellite side to the IF `if_khz` (dt295_init first, once).
 * This also turns the terrestrial side's TS off; dt295_tune turns it back
 * on.  0 ok, -1 the bus failed. */
int dt295_sat_tune(fe_xfer_fn x, void *ctx, unsigned if_khz);

/* The five bytes for tuner registers 0x00..0x04 (exposed for the tests). */
void dt295_sat_freq_bytes(unsigned if_khz, uint8_t out[5]);

/* One step of picking the TS after dt295_sat_tune; call every ~10 ms until
 * *locked.  `ts` 0..7 is the n-th TS the transponder lists (BS carries
 * several), anything else a TSID.  *picked starts at 0 and is kept between
 * calls; when locked it is the TSID.  0 ok, -1 the bus failed. */
int dt295_sat_poll(fe_xfer_fn x, void *ctx, unsigned ts, unsigned *picked,
                   int *locked);

/* Satellite lock (1/0) and the raw C/N (demod 0x11 0xBC..0xBD; bigger is
 * worse).  0 ok, -1 the bus failed. */
int dt295_sat_status(fe_xfer_fn x, void *ctx, int *locked, uint32_t *cnr);

#endif
