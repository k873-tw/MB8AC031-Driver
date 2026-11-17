/* test_stream.c -- the packet boundary and the host ring, no device.
 *
 * SPDX-License-Identifier: MIT
 */
/* The checks are the test: keep them in a Release build too. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devpriv.h"

#define P 188u

static void setup(struct mb8_stream *s, size_t cap)
{
    memset(s, 0, sizeof(*s));
    mb8_mutex_init(&s->lock);
    mb8_cond_init(&s->more);
    s->ring.buf = malloc(cap);
    s->ring.cap = cap;
    s->work = malloc(65536 + P);
    assert(s->ring.buf && s->work);
}

static void teardown(struct mb8_stream *s)
{
    free(s->ring.buf);
    free(s->work);
    mb8_cond_destroy(&s->more);
    mb8_mutex_destroy(&s->lock);
}

/* n packets, packet i filled with (i & 0xff) after the sync byte. */
static uint8_t *packets(unsigned n)
{
    uint8_t *b = malloc((size_t)n * P);
    unsigned i;

    for (i = 0; i < n; i++) {
        memset(b + (size_t)i * P, (int)(i & 0xffu), P);
        b[(size_t)i * P] = 0x47;
    }
    return b;
}

static void check_out(struct mb8_stream *s, unsigned first, unsigned n)
{
    uint8_t *out = malloc((size_t)n * P);
    size_t got = mb8__ring_get(&s->ring, out, (size_t)n * P);
    unsigned i;

    assert(got == (size_t)n * P);
    for (i = 0; i < n; i++) {
        assert(out[(size_t)i * P] == 0x47);
        assert(out[(size_t)i * P + 1] == (uint8_t)((first + i) & 0xffu));
    }
    free(out);
}

/* Starts mid-packet; split across feeds at awkward places. */
static void test_split(void)
{
    struct mb8_stream s;
    uint8_t *b = packets(40);
    size_t at = 77, cuts[] = { 1, 186, 188, 189, 5000 };
    unsigned i;

    setup(&s, 100 * P);
    for (i = 0; at < 40 * P; i++) {
        size_t k = cuts[i % 5];

        if (at + k > 40 * P)
            k = 40 * P - at;
        mb8__stream_feed(&s, b + at, k);
        at += k;
    }
    /* packet 0 was cut; 1..39 arrive.  The last one has no successor, so
     * it is accepted on the strength of its own sync once synced. */
    assert(s.st.dropped_bytes == P - 77);
    assert(s.st.resyncs == 0);
    check_out(&s, 1, 39);
    assert(s.ring.len == 0);
    teardown(&s);
    free(b);
}

/* A lost sync is found again and counted. */
static void test_resync(void)
{
    struct mb8_stream s;
    uint8_t *b = packets(10);

    setup(&s, 100 * P);
    b[4 * P] = 0x00;            /* packet 4 broken */
    mb8__stream_feed(&s, b, 10 * P);
    assert(s.st.resyncs == 1);
    /* 0..3, then the search skips packet 4 and picks up at 5 */
    assert(s.st.packets == 9);
    check_out(&s, 0, 4);
    check_out(&s, 5, 5);
    teardown(&s);
    free(b);
}

/* A full ring drops whole packets and counts the bytes. */
static void test_overflow(void)
{
    struct mb8_stream s;
    uint8_t *b = packets(10);

    setup(&s, 4 * P);
    mb8__stream_feed(&s, b, 10 * P);
    assert(s.st.packets == 4);
    assert(s.st.overflow_bytes == 6 * P);
    check_out(&s, 0, 4);
    /* wraps */
    mb8__stream_feed(&s, b, 3 * P);
    check_out(&s, 0, 3);
    mb8__stream_feed(&s, b + 3 * P, 3 * P);
    check_out(&s, 3, 3);
    teardown(&s);
    free(b);
}

int main(void)
{
    test_split();
    test_resync();
    test_overflow();
    printf("test_stream: ok\n");
    return 0;
}
