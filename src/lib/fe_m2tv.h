/* fe_m2tv.h -- GV-M2TV front end (terrestrial), tuned by the host.
 *
 * SPDX-License-Identifier: MIT
 *
 * Every write here was kept because taking it out, or flipping one of its
 * bits, changed something on a real GV-M2TV with CFW (lock, C/N or the
 * TS).  fe_m2tv.c says, write by write, what was seen;
 * docs/tuning-m2tv.md has the same as tables.
 */
#ifndef MB8_FE_M2TV_H
#define MB8_FE_M2TV_H

#include <stdint.h>

#include "fe.h"

#define M2TV_DEMOD      0x18u   /* 7-bit                                  */
#define M2TV_KHZ_MIN    470000u
#define M2TV_KHZ_MAX    700000u /* above it: not measured, refused        */

/* Once after the device comes up.  0 ok, -1 the bus failed. */
int m2tv_init(fe_xfer_fn x, void *ctx);

/* Tune to `khz`.  0 ok, -1 the bus failed. */
int m2tv_tune(fe_xfer_fn x, void *ctx, unsigned khz);

/* The tuner's frequency word for `khz` (exposed for the tests). */
unsigned m2tv_freq_word(unsigned khz);

/* Lock (1/0) and the raw C/N (demod 0x8B..0x8D; smaller is better).
 * 0 ok, -1 the bus failed. */
int m2tv_status(fe_xfer_fn x, void *ctx, int *locked, uint32_t *cnr);

#endif
