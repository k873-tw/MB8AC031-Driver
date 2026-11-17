/* test_fe_m2tv.c -- the GV-M2TV sequence against a fake bus: the values
 * the registers end up with, from the power-up values, and the frequency
 * word.
 *
 * SPDX-License-Identifier: MIT
 */
/* The checks are the test: keep them in a Release build too. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fe_m2tv.h"

/* The fake demodulator: 256 registers; a write [reg v] sets one, a write
 * [reg] then a read returns from there.  [FE C0 reg v] goes to the tuner,
 * [FE C0 FF] resets it. */
struct bus {
    uint8_t demod[256], tuner[256];
    unsigned writes, tuner_resets;
    int tuned_after_reset;      /* a frequency write came after the reset */
};

static int fake(void *ctx, unsigned addr7, const uint8_t *w, unsigned wn,
                uint8_t *r, unsigned rn)
{
    struct bus *b = ctx;
    unsigned i;

    assert(addr7 == M2TV_DEMOD);
    assert(wn >= 1);
    if (w[0] == 0xFE) {
        assert(wn >= 3 && w[1] == 0xC0 && rn == 0);
        if (wn == 3) {
            assert(w[2] == 0xFF);
            memset(b->tuner, 0, sizeof(b->tuner));
            b->tuner[0x02] = 0x06;      /* power-up values that matter */
            b->tuner_resets++;
            return 0;
        }
        for (i = 3; i < wn; i++)
            b->tuner[(w[2] + i - 3) & 0xFF] = w[i];
        if (w[2] == 0x0D)
            b->tuned_after_reset = 1;
        b->writes++;
        return 0;
    }
    if (wn == 1) {
        for (i = 0; i < rn; i++)
            r[i] = b->demod[(w[0] + i) & 0xFF];
        return 0;
    }
    assert(rn == 0);
    for (i = 1; i < wn; i++)
        b->demod[(w[0] + i - 1) & 0xFF] = w[i];
    b->writes++;
    return 0;
}

static void power_up(struct bus *b)
{
    memset(b, 0, sizeof(*b));
    /* the demodulator's measured power-up values, for the registers the
     * sequence touches */
    b->demod[0x11] = 0x21; b->demod[0x12] = 0x0B; b->demod[0x13] = 0x2C;
    b->demod[0x14] = 0x0A; b->demod[0x16] = 0x03; b->demod[0x1C] = 0x55;
    b->demod[0x1E] = 0x54; b->demod[0x1F] = 0x54; b->demod[0x30] = 0x28;
    b->demod[0x31] = 0x0F; b->demod[0x34] = 0x3F;
    b->tuner[0x02] = 0x06;
}

int main(void)
{
    struct bus b;
    int locked;
    uint32_t cnr;

    /* 1/64 MHz, rounded: UHF 13 ch and 27 ch */
    assert(m2tv_freq_word(473143u) == 0x7649u);
    assert(m2tv_freq_word(557143u) == 0x8B49u);

    power_up(&b);
    assert(m2tv_init(fake, &b) == 0);
    assert(b.tuner_resets == 1);
    /* the bits that were measured to matter */
    assert((b.demod[0x11] & 0x07) == 6 && (b.demod[0x11] & 0xF8) == 0x20);
    assert((b.demod[0x12] & 0x07) == 4);
    assert((b.demod[0x13] & 0x07) == 3);
    assert(b.demod[0x14] & 0x40);
    assert((b.demod[0x16] & 0x03) == 0);
    assert((b.demod[0x30] & 0x08) == 0);
    assert((b.demod[0x31] & 0x02) == 0);
    assert((b.demod[0x34] & 0x30) != 0x30);
    assert((b.demod[0x1C] & 0x40) == 0);
    assert((b.demod[0x1E] & 0x14) == 0 && (b.demod[0x1F] & 0x50) == 0);
    assert((b.tuner[0x02] & 0x04) == 0);
    assert(b.tuner[0x01] == 0x01);
    assert(b.demod[0x75] & 0x08);
    /* free bits stay as the chip has them */
    assert(b.demod[0x14] == 0x4A && b.demod[0x30] == 0x20);

    assert(m2tv_tune(fake, &b, 473143u) == 0);
    assert(b.tuned_after_reset);
    assert(b.tuner[0x0D] == 0x49 && b.tuner[0x0E] == 0x76);
    assert(b.tuner[0x0F] == 0x01);
    assert(b.demod[0x01] == 0x40);

    /* lock: 0x80 without bits 5/3, 0xB0 low nibble >= 8, 0x96 bits 7..5 */
    b.demod[0x80] = 0xAF; b.demod[0xB0] = 0xA0; b.demod[0x96] = 0x40;
    b.demod[0x8B] = 0x00; b.demod[0x8C] = 0x16; b.demod[0x8D] = 0x00;
    assert(m2tv_status(fake, &b, &locked, &cnr) == 0 && !locked);
    b.demod[0x80] = 0x06; b.demod[0xB0] = 0xA9;
    assert(m2tv_status(fake, &b, &locked, &cnr) == 0 && locked);
    assert(cnr == 0x1600u);
    b.demod[0x80] = 0x08;
    assert(m2tv_status(fake, &b, &locked, NULL) == 0 && !locked);
    b.demod[0x80] = 0x00; b.demod[0xB0] = 0xA7;
    assert(m2tv_status(fake, &b, &locked, NULL) == 0 && !locked);
    b.demod[0xB0] = 0xA9; b.demod[0x96] = 0x02;
    assert(m2tv_status(fake, &b, &locked, NULL) == 0 && !locked);
    b.demod[0x96] = 0x86;
    assert(m2tv_status(fake, &b, &locked, NULL) == 0 && locked);

    printf("test_fe_m2tv: ok\n");
    return 0;
}
