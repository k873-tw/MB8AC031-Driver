/* fe_dt295.c -- PIX-DT295 front end: terrestrial, then satellite.
 *
 * SPDX-License-Identifier: MIT
 *
 * The parts: a demodulator at 7-bit 0x10 and, behind it, a tuner.  A write
 * to the demodulator that starts FE C0 is passed on to the tuner, so
 * [FE C0 reg value] writes one tuner register.
 *
 * Method, on a PIX-DT295 running CFW and on a strong and a weak UHF
 * channel: every write below was taken out on its own, and then each bit
 * of each remaining write was flipped, one at a time.  The writes left are
 * the ones needed for the same lock, the same C/N and a clean TS (no
 * resync, no continuity jump, no TEI, PAT present).  The comment on each
 * write says what happens without it.  "Free" bits changed nothing when
 * flipped.
 *
 * Measured from a power-up only for the registers listed; the demodulator
 * has no reset that we know of, so dt295_init also puts back the bits
 * whose power-up value is the one that works (0x03, 0x23).
 */
#include "fe_dt295.h"

#include <stddef.h>

#define DEMOD   DT295_DEMOD_T

static int dw(fe_xfer_fn x, void *ctx, unsigned reg, unsigned v)
{
    uint8_t b[2] = { (uint8_t)reg, (uint8_t)v };
    return x(ctx, DEMOD, b, 2, NULL, 0);
}

static int dr(fe_xfer_fn x, void *ctx, unsigned reg, uint8_t *out,
              unsigned n)
{
    uint8_t b = (uint8_t)reg;
    return x(ctx, DEMOD, &b, 1, out, n);
}

/* Read, clear `clear`, set `set`, write back. */
static int dmod(fe_xfer_fn x, void *ctx, unsigned reg, unsigned clear,
                unsigned set)
{
    uint8_t v;
    if (dr(x, ctx, reg, &v, 1) != 0)
        return -1;
    return dw(x, ctx, reg, (v & ~clear & 0xFFu) | set);
}

/* One tuner register, through the demodulator. */
static int tw(fe_xfer_fn x, void *ctx, unsigned reg, unsigned v)
{
    uint8_t b[4] = { 0xFE, 0xC0, (uint8_t)reg, (uint8_t)v };
    return x(ctx, DEMOD, b, 4, NULL, 0);
}

int dt295_init(fe_xfer_fn x, void *ctx)
{
    static const uint8_t reset[3] = { 0xFE, 0xC0, 0xFF };

    /* The tuner first, then the demodulator; each in register order. */

    /* Tuner register 0xFF, no data: a reset.  Read back afterwards, every
     * tuner register is at its power-up value, so the writes below start
     * from known values. */
    if (x(ctx, DEMOD, reset, 3, NULL, 0) != 0)
        return -1;
    /* Tuner 0x03: bit 0 must be 1 (0: no lock).  Other bits free. */
    if (tw(x, ctx, 0x03, 0x01) != 0)
        return -1;
    /* Tuner 0x05: power-up 0xDB.  Bit 6 must be 0 (set: no lock); bit 4
     * must stay 1 (cleared: no lock); bit 3 must stay 1 (cleared: C/N
     * 1.6..4.9 dB worse).  Bits 7, 5, 2..0 free. */
    if (tw(x, ctx, 0x05, 0x1B) != 0)
        return -1;
    /* Tuner 0x08: power-up 0x01.  Bit 2 set raises the C/N by 2..4.7 dB;
     * bit 0 must stay 1 and bit 3 stay 0 (either changed: no lock). */
    if (tw(x, ctx, 0x08, 0x05) != 0)
        return -1;

    /* Demod 0x03: bits 6..4 must be 0 (any one set: no lock); 0 at
     * power-up.  Cleared here for a front end that was set up before. */
    if (dmod(x, ctx, 0x03, 0x70, 0x00) != 0)
        return -1;
    /* Demod 0x1C: bits 7..6 must be 0 (either one set: no lock).  Its
     * power-up value 0x50 has bit 6 set.  Bits 5..0 free. */
    if (dw(x, ctx, 0x1C, 0x20) != 0)
        return -1;
    /* Demod 0x23: bits 1..0 must be 0 (either one set: no lock); 0 at
     * power-up.  Same reason as 0x03. */
    if (dmod(x, ctx, 0x23, 0x03, 0x00) != 0)
        return -1;
    return 0;
}

unsigned dt295_freq_word(unsigned khz)
{
    /* The tuner takes the frequency in 1/64 MHz. */
    return (unsigned)(((uint64_t)khz * 64u) / 1000u) & 0xFFFFu;
}

int dt295_tune(fe_xfer_fn x, void *ctx, unsigned khz)
{
    unsigned n = dt295_freq_word(khz);

    /* Tuner 0x0B: bit 0 must be 1 (0: no lock).  Other bits free. */
    if (tw(x, ctx, 0x0B, 0x01) != 0)
        return -1;
    /* Tuner 0x10 / 0x11: the frequency word, low byte then high. */
    if (tw(x, ctx, 0x10, n & 0xFFu) != 0 ||
        tw(x, ctx, 0x11, n >> 8) != 0)
        return -1;
    /* Tuner 0x12: 0 then 1 makes the tuner take the new frequency.  With
     * no 0 first, a second tune stays on the old channel; with no 1, no
     * lock. */
    if (tw(x, ctx, 0x12, 0x00) != 0 ||
        tw(x, ctx, 0x12, 0x01) != 0)
        return -1;
    /* Then the demodulator, in register order. */
    /* Demod 0x01: bit 6 set locks in about 0.65 s instead of 1.1..1.3 s;
     * bit 7 must be 0 (set: no lock).  Bits 5..0 free. */
    if (dw(x, ctx, 0x01, 0x40) != 0)
        return -1;
    /* Demod 0x0E: bits 2..0 must be 001 (power-up 111; any other value:
     * lock, but no TS).  Bits 7..3 free, kept. */
    if (dmod(x, ctx, 0x0E, 0x07, 0x01) != 0)
        return -1;
    /* Demod 0x1D: bits 7..3 must be 0 (any one set: lock, but no TS); the
     * power-up 0x55 has bits 6 and 4 set.  Bits 2..0 free. */
    if (dw(x, ctx, 0x1D, 0x02) != 0)
        return -1;
    return 0;
}

int dt295_status(fe_xfer_fn x, void *ctx, int *locked, uint32_t *cnr)
{
    uint8_t s, c[3];

    /* Demod 0x80 reads 0x00 from the moment the signal is locked (before:
     * 0x2F, then 0x2E, 0x04). */
    if (dr(x, ctx, 0x80, &s, 1) != 0)
        return -1;
    /* Demod 0x8B..0x8D: the C/N, 24 bits.  Smaller is better. */
    if (dr(x, ctx, 0x8B, c, 3) != 0)
        return -1;
    if (locked)
        *locked = s == 0;
    if (cnr)
        *cnr = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
    return 0;
}

/* --- satellite -----------------------------------------------------------
 *
 * The parts: a second demodulator at 7-bit 0x11 and its own tuner behind
 * it, reached the same way ([FE C0 reg value...] to 0x11).  Both TS go to
 * the same pins; the terrestrial demodulator's 0x0E picks whether it
 * drives them.
 *
 * Method, on a PIX-DT295 running CFW: every write below was taken out on
 * its own, and then each bit of each remaining write was flipped.  Before
 * each trial every register either chip was written in was put back to
 * its power-up value (neither chip has a reset we know of).  Checked on BS
 * and CS transponders that lock, and when switching terrestrial ->
 * satellite -> terrestrial -> satellite.
 * "Free" bits changed nothing when flipped.
 */

static int sw(fe_xfer_fn x, void *ctx, unsigned reg, unsigned v)
{
    uint8_t b[2] = { (uint8_t)reg, (uint8_t)v };
    return x(ctx, DT295_DEMOD_S, b, 2, NULL, 0);
}

static int sr(fe_xfer_fn x, void *ctx, unsigned reg, uint8_t *out,
              unsigned n)
{
    uint8_t b = (uint8_t)reg;
    return x(ctx, DT295_DEMOD_S, &b, 1, out, n);
}

/* `n` satellite-tuner registers from `reg` on, in one write. */
static int stw(fe_xfer_fn x, void *ctx, unsigned reg, const uint8_t *v,
               unsigned n)
{
    uint8_t b[3 + 8];
    unsigned i;

    if (n > 8)
        return -1;
    b[0] = 0xFE;
    b[1] = 0xC0;
    b[2] = (uint8_t)reg;
    for (i = 0; i < n; i++)
        b[3 + i] = v[i];
    return x(ctx, DT295_DEMOD_S, b, 3 + n, NULL, 0);
}

static int stw1(fe_xfer_fn x, void *ctx, unsigned reg, unsigned v)
{
    uint8_t b = (uint8_t)v;
    return stw(x, ctx, reg, &b, 1);
}

void dt295_sat_freq_bytes(unsigned if_khz, uint8_t out[5])
{
    /* The tuner takes the frequency in MHz, rounded (registers 0x00..0x01:
     * the MHz in bits 15..1).  Below 1,101 MHz it takes twice the MHz with
     * bit 7 of register 0x04 set.  Both ways measured: BS 1 and BS 3 lie
     * below, everything from BS 5 up above. */
    unsigned mhz = (if_khz + 500u) / 1000u;
    uint8_t hi = 0x00;

    if (if_khz + 500u < 1101000u) {
        mhz *= 2u;
        hi = 0x80;
    }
    out[0] = (uint8_t)(mhz >> 7);
    out[1] = (uint8_t)(mhz << 1);
    out[2] = 0x00;
    out[3] = 0x00;
    out[4] = hi;
}

int dt295_sat_tune(fe_xfer_fn x, void *ctx, unsigned if_khz)
{
    static const uint8_t r06[2] = { 0x10, 0x10 };
    uint8_t f[5], v;

    dt295_sat_freq_bytes(if_khz, f);

    /* The demodulator, then the tuner, each in register order, with the
     * tuner's frequency (0x00..0x04) and then 0x05 and 0x37 last; then the
     * TS pins.  Two of the tuner's orderings matter (measured on a
     * PIX-DT295, the first satellite tune after the event named):
     *   - 0x06..0x2A before the frequency.  With them after it, the first
     *     tune after power-up did not lock (the next one did).
     *   - 0x05 and 0x37 after the frequency.  With them before it, the
     *     first tune after a terrestrial one did not lock (the next one
     *     did).
     * Some of these registers are right at power-up but other software can
     * leave them wrong, and are written back to their power-up value: for
     * those, the comment names a wrong value and what happens when that
     * register alone stays there.  Each write is needed. */

    /* Demod 0x04: bit 1 must be 1 (0: BS loses packets, resyncs and TEI
     * on every one; CS unaffected).  Power-up 0x00.  Other bits free. */
    if (sw(x, ctx, 0x04, 0x02) != 0)
        return -1;
    /* Demod 0x07 (power-up value).  Left at 0x33: lock, but no TS. */
    if (sw(x, ctx, 0x07, 0x01) != 0)
        return -1;
    /* Demod 0x0A (power-up value).  Left at 0x00: no lock.  (It reads back
     * 0x00 after this write all the same; the write is what counts.) */
    if (sw(x, ctx, 0x0A, 0xFF) != 0)
        return -1;
    /* Demod 0x17 (power-up value).  Left at 0x01: no lock. */
    if (sw(x, ctx, 0x17, 0x00) != 0)
        return -1;
    /* Demod 0x1E: bits 3..2 must be 0 (either set: no lock on BS, CS at
     * 5.8 dB); the power-up 0x14 has bit 2 set.  Other bits free. */
    if (sw(x, ctx, 0x1E, 0x00) != 0)
        return -1;
    /* Demod 0x87: power-up 0x00.  Bit 2 must be 1 (0: no lock on BS, CS
     * at 5.8 dB); bits 7 and 5 must be 0 (set: no lock); bit 3 must be 0
     * (set: C/N 0.8..1.5 dB worse), bit 4 better 0 (set: C/N 0.6..0.9 dB
     * worse); bit 0 must be 0 (set: the C/N reads 0).  Bits 6, 1 free. */
    if (sw(x, ctx, 0x87, 0x04) != 0)
        return -1;


    /* Tuner 0x06, 0x07: 0x10 each; flipping any one of the 16 bits: no
     * lock.  Power-up 0x1B, 0x1B. */
    if (stw(x, ctx, 0x06, r06, 2) != 0)
        return -1;
    /* Tuner 0x11 (power-up value).  Left at 0xC0: no lock. */
    if (stw1(x, ctx, 0x11, 0x00) != 0)
        return -1;
    /* Tuner 0x1C (power-up value).  Left at 0x00: no lock. */
    if (stw1(x, ctx, 0x1C, 0xC0) != 0)
        return -1;
    /* Tuner 0x29 (power-up value).  Left at 0x70: no lock. */
    if (stw1(x, ctx, 0x29, 0x71) != 0)
        return -1;
    /* Tuner 0x2A (power-up value).  Left at 0xB9: no lock. */
    if (stw1(x, ctx, 0x2A, 0xBB) != 0)
        return -1;

    /* The frequency, then 0x05 and 0x37 (see above). */
    /* Tuner 0x00..0x04: the frequency. */
    if (stw(x, ctx, 0x00, f, 5) != 0)
        return -1;
    /* Tuner 0x05: bit 7 must be 1 (0: no lock on BS; CS unaffected).
     * Power-up 0x00.  Other bits free. */
    if (stw1(x, ctx, 0x05, 0x80) != 0)
        return -1;
    /* Tuner 0x37: power-up 0x86.  Bit 0 must be 0 (1: no lock); bit 5
     * must be 1 (0: no lock on BS, CS at 5.3 dB); bits 7 and 6 as here
     * (else C/N 2.2..4.4 dB worse), bit 4 as here (else C/N 0.5..1.6 dB
     * worse).  Bits 3..1 free. */
    if (stw1(x, ctx, 0x37, 0xAC) != 0)
        return -1;

    /* Terrestrial demod 0x0E: bits 2..0 = 111 takes the terrestrial side
     * off the TS pins (any one 0: lock, but no TS).  Bits 7..3 free, kept.
     * dt295_tune puts 001 back. */
    {
        uint8_t r = 0x0E, w[2];
        if (x(ctx, DT295_DEMOD_T, &r, 1, &v, 1) != 0)
            return -1;
        w[0] = 0x0E;
        w[1] = (uint8_t)(v | 0x07u);
        if (x(ctx, DT295_DEMOD_T, w, 2, NULL, 0) != 0)
            return -1;
    }
    /* Demod 0x1C: bit 7 must be 0 (set: lock, but no TS).  The power-up
     * 0x55 would do; written for a front end set up before.  Other bits
     * free. */
    if (sw(x, ctx, 0x1C, 0x08) != 0)
        return -1;
    /* Demod 0x1F: bits 5, 4, 1, 0 must be 0 (any one set: lock, but no
     * TS); the power-up 0x15 has bits 4 and 0 set.  Bits 7, 6, 3, 2 free. */
    if (sw(x, ctx, 0x1F, 0x08) != 0)
        return -1;
    /* Demod 0x8E bit 1: drop the dummy slots.  Clear (the power-up 0x00),
     * a CS transponder comes out at about 6.5 MB/s with 31-33 % null
     * packets -- more than the endpoint FIFO takes, so continuity breaks
     * once the ring fills.  Set: 8-10 % null and 4.88 MB/s on CS 2, 4, 8,
     * 14 and 20, no continuity jumps; the same TSID, and no change on BS
     * 1, 3 and 13 (the BS transponders already came out at or below
     * 3.3 MB/s).  Bits 5 and 2 changed nothing measured; written 0. */
    if (sw(x, ctx, 0x8E, 0x02) != 0)
        return -1;
    return 0;
}

int dt295_sat_poll(fe_xfer_fn x, void *ctx, unsigned ts, unsigned *picked,
                   int *locked)
{
    uint8_t c3, c5, list[16], e6[2];
    unsigned i, n = 0, pick = 0;

    *locked = 0;
    /* Demod 0xC3 goes up after a tune and back to 0x00 once the
     * transponder's list of TS is read.  Seen at 0x01 or 0x10 while the
     * list below still held the previous transponder's. */
    if (sr(x, ctx, 0xC3, &c3, 1) != 0)
        return -1;
    if (c3 != 0)
        return 0;
    /* Demod 0xCE..0xDD: up to eight TSIDs, big-endian, 0xFFFF unused.  BS
     * lists its TS in order (BS 1: 0x4010, 0x4011, 0x4012). */
    if (sr(x, ctx, 0xCE, list, 16) != 0)
        return -1;
    for (i = 0; i < 8; i++) {
        unsigned v = ((unsigned)list[2 * i] << 8) | list[2 * i + 1];
        if (v == 0xFFFFu || v == 0)
            continue;
        if (ts < 8u ? n == ts : v == ts)
            pick = v;
        n++;
    }
    if (pick == 0)
        return 0;
    /* Demod 0x8F..0x90: the TSID to put out.  Written again when the list
     * changes under us (it was the old transponder's). */
    if (*picked != pick) {
        uint8_t w[3] = { 0x8F, (uint8_t)(pick >> 8), (uint8_t)pick };
        if (x(ctx, DT295_DEMOD_S, w, 3, NULL, 0) != 0)
            return -1;
        *picked = pick;
        return 0;
    }
    /* Demod 0xC5 bit 1: the TS is coming; 0xE6..0xE7: the TSID it is. */
    if (sr(x, ctx, 0xC5, &c5, 1) != 0 || sr(x, ctx, 0xE6, e6, 2) != 0)
        return -1;
    *locked = (c5 & 0x02u) &&
              (((unsigned)e6[0] << 8) | e6[1]) == pick;
    return 0;
}

int dt295_sat_status(fe_xfer_fn x, void *ctx, int *locked, uint32_t *cnr)
{
    uint8_t c3, c5, c[2];

    if (sr(x, ctx, 0xC3, &c3, 1) != 0 || sr(x, ctx, 0xC5, &c5, 1) != 0)
        return -1;
    /* Demod 0xBC..0xBD: the C/N, 16 bits, bigger is worse. */
    if (sr(x, ctx, 0xBC, c, 2) != 0)
        return -1;
    if (locked)
        *locked = c3 == 0 && (c5 & 0x02u);
    if (cnr)
        *cnr = ((uint32_t)c[0] << 8) | c[1];
    return 0;
}
