/* test_fe_dt295.c -- the PIX-DT295 sequence against a fake bus: the order
 * of the writes, the read-modify-writes and the frequency word.
 *
 * SPDX-License-Identifier: MIT
 */
/* The checks are the test: keep them in a Release build too. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fe_dt295.h"

/* The fake demodulator: 256 registers; a write [reg v] sets one, a write
 * [reg] then a read returns from there.  [FE C0 reg v] goes to the tuner. */
struct bus {
    uint8_t demod[256], tuner[256];
    uint8_t sdemod[256], stuner[256];
    char log[64][24];           /* "d1C=20", "t12=01", "tFF" in order     */
    unsigned n;
};

static void logf_(struct bus *b, const char *s)
{
    assert(b->n < 64);
    snprintf(b->log[b->n++], sizeof(b->log[0]), "%s", s);
}

static int fake(void *ctx, unsigned addr7, const uint8_t *w, unsigned wn,
                uint8_t *r, unsigned rn)
{
    struct bus *b = ctx;
    char s[24];
    unsigned i;

    assert(addr7 == DT295_DEMOD_T || addr7 == DT295_DEMOD_S);
    assert(wn >= 1);
    if (addr7 == DT295_DEMOD_S) {
        if (wn >= 3 && w[0] == 0xFE && w[1] == 0xC0) {
            int k;
            assert(rn == 0);
            k = snprintf(s, sizeof(s), "T%02X=", w[2]);
            for (i = 3; i < wn; i++) {
                b->stuner[(w[2] + i - 3) & 0xFF] = w[i];
                k += snprintf(s + k, sizeof(s) - (size_t)k, "%02X", w[i]);
            }
            logf_(b, s);
            return 0;
        }
        if (wn == 1) {
            for (i = 0; i < rn; i++)
                r[i] = b->sdemod[(w[0] + i) & 0xFF];
            return 0;
        }
        assert(rn == 0);
        {
            int k = snprintf(s, sizeof(s), "S%02X=", w[0]);
            for (i = 1; i < wn; i++) {
                b->sdemod[(w[0] + i - 1) & 0xFF] = w[i];
                k += snprintf(s + k, sizeof(s) - (size_t)k, "%02X", w[i]);
            }
        }
        logf_(b, s);
        return 0;
    }
    if (wn >= 3 && w[0] == 0xFE && w[1] == 0xC0) {
        assert(rn == 0);
        if (wn == 3) {
            assert(w[2] == 0xFF);
            memset(b->tuner, 0, sizeof(b->tuner));
            logf_(b, "tFF");
            return 0;
        }
        assert(wn == 4);
        b->tuner[w[2]] = w[3];
        snprintf(s, sizeof(s), "t%02X=%02X", w[2], w[3]);
        logf_(b, s);
        return 0;
    }
    if (wn == 1) {
        for (i = 0; i < rn; i++)
            r[i] = b->demod[(w[0] + i) & 0xFF];
        return 0;
    }
    assert(wn == 2 && rn == 0);
    b->demod[w[0]] = w[1];
    snprintf(s, sizeof(s), "d%02X=%02X", w[0], w[1]);
    logf_(b, s);
    return 0;
}

static void expect(const struct bus *b, const char *const *want, unsigned n)
{
    unsigned i;
    if (b->n != n)
        fprintf(stderr, "got %u writes, want %u\n", b->n, n);
    for (i = 0; i < n && i < b->n; i++)
        if (strcmp(b->log[i], want[i]) != 0)
            fprintf(stderr, "write %u: got %s, want %s\n", i, b->log[i],
                    want[i]);
    assert(b->n == n);
    for (i = 0; i < n; i++)
        assert(strcmp(b->log[i], want[i]) == 0);
}

int main(void)
{
    static const char *const init[] = {
        "tFF", "t03=01", "t05=1B", "t08=05", "d03=80", "d1C=20", "d23=4C",
    };
    static const char *const tune[] = {
        "t0B=01", "t10=49", "t11=8B", "t12=00", "t12=01", "d01=40",
        "d0E=71", "d1D=02",
    };
    struct bus b;
    uint32_t cnr;
    int locked;

    /* Frequency word: 1/64 MHz, truncated. */
    assert(dt295_freq_word(557143u) == 0x8B49u);
    assert(dt295_freq_word(473143u) == 0x7649u);

    /* Init from a front end someone set up before: 0x03 = 0x90, 0x23 =
     * 0x4D.  The bits that stop the lock come off, the rest stay. */
    memset(&b, 0, sizeof(b));
    b.demod[0x03] = 0x90;
    b.demod[0x23] = 0x4D;
    b.demod[0x0E] = 0x77;
    assert(dt295_init(fake, &b) == 0);
    expect(&b, init, sizeof(init) / sizeof(init[0]));

    /* Tune to UHF 27 ch: 0x0E keeps its upper bits, low three to 001. */
    b.n = 0;
    assert(dt295_tune(fake, &b, 557143u) == 0);
    expect(&b, tune, sizeof(tune) / sizeof(tune[0]));
    assert(b.tuner[0x12] == 0x01);

    /* Status: the firmware's lock rule -- 0x80 with no bit of 0x28, and
     * 0xB0's low nibble >= 8; 0x8B..0x8D the C/N. */
    b.demod[0x80] = 0x2E;
    b.demod[0x8B] = 0x00; b.demod[0x8C] = 0x07; b.demod[0x8D] = 0x89;
    assert(dt295_status(fake, &b, &locked, &cnr) == 0);
    assert(!locked && cnr == 0x789u);
    b.demod[0x80] = 0x00;
    b.demod[0xB0] = 0xA9;
    assert(dt295_status(fake, &b, &locked, NULL) == 0);
    assert(locked);
    /* 13ch as measured: 0x80 stops at 0x04, 0xB0 at 0xA9 -- locked. */
    b.demod[0x80] = 0x04;
    assert(dt295_status(fake, &b, &locked, NULL) == 0);
    assert(locked);
    /* Sync bits clear but 0xB0's low nibble below 8: not locked. */
    b.demod[0xB0] = 0xA7;
    assert(dt295_status(fake, &b, &locked, NULL) == 0);
    assert(!locked);

    /* Satellite: the frequency bytes on both sides of 1,101 MHz. */
    {
        uint8_t f[5];
        dt295_sat_freq_bytes(1049480u, f);      /* BS 1: 1049 MHz, x2 */
        assert(f[0] == 0x10 && f[1] == 0x64 && f[2] == 0 && f[3] == 0 &&
               f[4] == 0x80);
        dt295_sat_freq_bytes(1613000u, f);      /* CS 2: 1613 MHz */
        assert(((f[0] << 7) | (f[1] >> 1)) == 1613 && f[4] == 0x00);
        dt295_sat_freq_bytes(1100499u, f);      /* rounds to 1100: x2 */
        assert(f[4] == 0x80);
        dt295_sat_freq_bytes(1100500u, f);      /* 1101: not */
        assert(f[4] == 0x00);
    }
    {
        static const char *const stune[] = {
            "S04=02", "S07=01", "S0A=FF", "S17=00", "S1E=00", "S87=04",
            "T06=1010", "T11=00", "T1C=C0", "T29=71", "T2A=BB",
            "T00=1064000080", "T05=80", "T37=AC", "d0E=77",
            "S1C=08", "S1F=08", "S8E=02",
        };
        unsigned picked = 0;
        uint32_t c2;

        b.n = 0;
        b.demod[0x0E] = 0x71;
        assert(dt295_sat_tune(fake, &b, 1049480u) == 0);
        expect(&b, stune, sizeof(stune) / sizeof(stune[0]));

        /* Pick: nothing until 0xC3 is 0; a stale list is picked, then
         * the right one once it changes. */
        b.n = 0;
        b.sdemod[0xC3] = 0x50;
        assert(dt295_sat_poll(fake, &b, 1, &picked, &locked) == 0);
        assert(!locked && picked == 0 && b.n == 0);
        b.sdemod[0xC3] = 0x00;
        memset(&b.sdemod[0xCE], 0xFF, 16);
        b.sdemod[0xCE] = 0x60; b.sdemod[0xCF] = 0x20;   /* old: CS 2 */
        assert(dt295_sat_poll(fake, &b, 1, &picked, &locked) == 0);
        assert(!locked && picked == 0);                 /* no 2nd TS */
        b.sdemod[0xCE] = 0x40; b.sdemod[0xCF] = 0x10;
        b.sdemod[0xD0] = 0x40; b.sdemod[0xD1] = 0x11;
        assert(dt295_sat_poll(fake, &b, 1, &picked, &locked) == 0);
        assert(!locked && picked == 0x4011);
        assert(b.n == 1 && strcmp(b.log[0], "S8F=4011") == 0);
        b.sdemod[0xC5] = 0x6E; b.sdemod[0xE6] = 0x40; b.sdemod[0xE7] = 0x10;
        assert(dt295_sat_poll(fake, &b, 1, &picked, &locked) == 0);
        assert(!locked);                                /* E6 not yet */
        b.sdemod[0xE7] = 0x11;
        assert(dt295_sat_poll(fake, &b, 1, &picked, &locked) == 0);
        assert(locked);
        /* By TSID. */
        picked = 0;
        assert(dt295_sat_poll(fake, &b, 0x4011, &picked, &locked) == 0);
        assert(picked == 0x4011);

        b.sdemod[0xBC] = 0x1E; b.sdemod[0xBD] = 0x9D;
        assert(dt295_sat_status(fake, &b, &locked, &c2) == 0);
        assert(locked && c2 == 0x1E9Du);
    }

    printf("test_fe_dt295: ok\n");
    return 0;
}
