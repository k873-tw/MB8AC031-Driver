/* fe.h -- what the front-end modules (fe_m2tv.c, fe_dt295.c) share.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MB8_FE_H
#define MB8_FE_H

#include <stdint.h>

/* One I2C transfer: write wn bytes, then (repeated START) read rn.  0 ok.
 * dev.c passes mb8_i2c; the tests pass a fake bus. */
typedef int (*fe_xfer_fn)(void *ctx, unsigned addr7, const uint8_t *w,
                          unsigned wn, uint8_t *r, unsigned rn);

#endif
