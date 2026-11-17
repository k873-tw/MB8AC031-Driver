/* stream.c -- the TS: a pump thread sends STREAM frames, a reader thread
 * keeps a bulk read waiting on EP 0x81, and whole 188-byte packets go into
 * a ring the consumer drains.
 *
 * SPDX-License-Identifier: MIT
 *
 * WHY TWO THREADS.  The firmware has no main loop: one STREAM frame moves
 * what it can and returns (MB8AC031-CFW docs/frq1.md).  On dione a frame
 * waits for room in the endpoint FIFO for every 512-byte chunk, so unless
 * the host is already reading the endpoint, the frame cannot finish.  On
 * selene a frame that meets a full FIFO gives up early.  Either way the
 * read must be outstanding while frames go out; libusb allows a bulk read
 * on one thread and control transfers on another.
 *
 * WHAT GOES OUT.  The stream starts in the middle of a packet.  The reader
 * looks for 0x47 with another 0x47 188 bytes on (or the end of what it
 * has), and from there hands over whole packets; on a lost sync it looks
 * again and counts it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devpriv.h"
#include "frq1.h"

#define TS_PKT          188u
#define READ_CHUNK      65536
#define READ_MS         200u
#define RING_DEFAULT    (32u << 20)
#define EP_TS           0x81u

/* --- the ring -------------------------------------------------------------
 * All under s->lock. */

static void ring_put(struct mb8_stream *s, const uint8_t *p, size_t n)
{
    struct mb8_ring *r = &s->ring;
    size_t room = r->cap - r->len;
    size_t tail, first;

    if (n > room) {             /* whole packets only: drop the excess */
        size_t keep = room - room % TS_PKT;

        s->st.overflow_bytes += n - keep;
        n = keep;
    }
    if (n == 0)
        return;
    tail = (r->head + r->len) % r->cap;
    first = r->cap - tail < n ? r->cap - tail : n;
    memcpy(r->buf + tail, p, first);
    memcpy(r->buf, p + first, n - first);
    r->len += n;
    s->st.packets += n / TS_PKT;
}

size_t mb8__ring_get(struct mb8_ring *r, uint8_t *p, size_t n)
{
    size_t first;

    if (n > r->len)
        n = r->len;
    n -= n % TS_PKT;
    first = r->cap - r->head < n ? r->cap - r->head : n;
    memcpy(p, r->buf + r->head, first);
    memcpy(p + first, r->buf, n - first);
    r->head = (r->head + n) % r->cap;
    r->len -= n;
    return n;
}

/* --- the packet boundary ------------------------------------------------- */

/* Feed raw bytes; whole packets go to the ring.  Reader thread only, except
 * for the ring itself. */
void mb8__stream_feed(struct mb8_stream *s, const uint8_t *p, size_t n)
{
    uint8_t *work = s->work;
    size_t have = 0, at = 0, dropped = 0;
    unsigned resyncs = 0;

    /* What did not make a whole packet last time goes in front.  It is
     * always shorter than a packet: an unsynced search eats everything it
     * passes, a synced run stops less than a packet from the end. */
    memcpy(work, s->carry, s->ncarry);
    have = s->ncarry;
    memcpy(work + have, p, n);
    have += n;
    s->ncarry = 0;

    mb8_mutex_lock(&s->lock);
    while (at < have) {
        if (!s->synced) {
            while (at < have && !(work[at] == 0x47u &&
                   (at + TS_PKT >= have || work[at + TS_PKT] == 0x47u))) {
                at++;
                dropped++;
            }
            if (at >= have)
                break;
            s->synced = 1;
        }
        if (have - at < TS_PKT)
            break;
        if (work[at] != 0x47u) {
            s->synced = 0;
            resyncs++;
            continue;
        }
        ring_put(s, work + at, TS_PKT);
        at += TS_PKT;
    }
    s->st.dropped_bytes += dropped;
    s->st.resyncs += resyncs;
    s->st.bytes_in += n;
    mb8_mutex_unlock(&s->lock);
    mb8_cond_broadcast(&s->more);

    if (at < have) {
        s->ncarry = have - at < sizeof(s->carry) ? have - at : 0;
        memcpy(s->carry, work + at, s->ncarry);
    }
}

static void die(mb8_dev *d, const char *why)
{
    struct mb8_stream *s = &d->s;

    mb8_mutex_lock(&s->lock);
    if (!s->failed) {
        s->failed = 1;
        snprintf(s->err, sizeof(s->err), "%s", why);
    }
    mb8_mutex_unlock(&s->lock);
    mb8_cond_broadcast(&s->more);
}

/* --- the threads ------------------------------------------------------- */

static void reader_main(void *arg)
{
    mb8_dev *d = arg;
    struct mb8_stream *s = &d->s;
    uint8_t *buf = malloc(READ_CHUNK);

    if (buf == NULL) {
        die(d, "reader: out of memory");
        return;
    }
    while (s->quit < 2) {
        int got = 0;
        int rc = usbio_bulk(&d->io, s->ep, buf, READ_CHUNK, READ_MS, &got);

        if (rc < 0) {
            die(d, d->io.err);
            break;
        }
        if (got > 0)
            mb8__stream_feed(s, buf, (size_t)got);
    }
    free(buf);
}

static void pump_main(void *arg)
{
    mb8_dev *d = arg;
    struct mb8_stream *s = &d->s;
    int dione = d->model->gen == MB8_GEN_DIONE;
    uint8_t out = FRQ1_DSO_EP81;
    uint8_t r[FRQ1_DATA_BYTES];

    while (s->quit < 1 && !s->failed) {
        int rc;

        if (dione)
            rc = mb8_call(d, FRQ1_CMD_STREAM, s->bursts, s->ring_mib, &out,
                          1, r);
        else
            rc = mb8_call(d, FRQ1_CMD_STREAM, s->bursts, 0, NULL, 0, r);
        if (rc != MB8_OK) {
            char why[320];

            snprintf(why, sizeof(why), "STREAM: %s (rc %d)", mb8_error(d),
                     (int)frq1_get32s(r, dione ? FRQ1_DSR_RC : FRQ1_SR_RC));
            die(d, why);
            break;
        }
        mb8_mutex_lock(&s->lock);
        s->st.frames++;
        if (dione && r[FRQ1_DSR_VERSION] >= 2)
            s->st.fw_fulls = r[FRQ1_DSR_FULLS];
        if (dione && r[FRQ1_DSR_VERSION] >= 3)
            s->st.fw_teis = frq1_get32(r, FRQ1_DSR_TEIS);
        mb8_mutex_unlock(&s->lock);
        /* selene: an empty ring answers at once; do not hammer EP0. */
        if (!dione && frq1_get32(r, FRQ1_SR_BYTES) == 0)
            mb8_sleep_ms(2u);
    }
}

/* --- the API ----------------------------------------------------------- */

int mb8_stream_start(mb8_dev *d, const struct mb8_stream_opts *o)
{
    struct mb8_stream *s = &d->s;
    size_t cap = o && o->buffer_bytes ? o->buffer_bytes : RING_DEFAULT;
    uint8_t scratch[4096];
    int got, rc;

    if (!(d->model->can & MB8_CAN_STREAM))
        return mb8__fail(d, MB8_E_UNSUPP, "%s: streaming has not been run "
                         "on this model", d->model->name);
    if (s->running)
        return mb8__fail(d, MB8_E_STATE, "the stream is already running");
    rc = mb8_hooked(d);
    if (rc != 1)
        return rc < 0 ? rc : MB8_E_NOCFW;
    if (usbio_claim(&d->io) != 0)
        return mb8__fail(d, MB8_E_USB, "%s", d->io.err);

    cap -= cap % TS_PKT;
    if (cap < 64u * TS_PKT)
        cap = 64u * TS_PKT;
    s->ring.buf = malloc(cap);
    s->work = malloc(READ_CHUNK + TS_PKT);
    if (s->ring.buf == NULL || s->work == NULL) {
        free(s->ring.buf);
        free(s->work);
        s->ring.buf = NULL;
        s->work = NULL;
        return MB8_E_NOMEM;
    }
    s->ring.cap = cap;
    s->ring.head = s->ring.len = 0;
    s->ep = EP_TS;
    s->bursts = o && o->bursts ? o->bursts : 0;
    s->ring_mib = o && o->ring_mib ? o->ring_mib : 0;
    if (d->model->gen == MB8_GEN_DIONE) {
        if (s->bursts > FRQ1_DSR_BURSTS_MAX)
            s->bursts = FRQ1_DSR_BURSTS_MAX;
        if (s->ring_mib > FRQ1_DSR_RING_MAX)
            s->ring_mib = FRQ1_DSR_RING_MAX;
    } else if (s->bursts == 0 || s->bursts > FRQ1_SR_BURSTS_MAX) {
        s->bursts = FRQ1_SR_BURSTS_MAX;
    }
    s->quit = 0;
    s->failed = 0;
    s->err[0] = '\0';
    s->ncarry = 0;
    s->synced = 0;
    memset(&s->st, 0, sizeof(s->st));

    /* Whatever an earlier run left in the FIFO is not this stream. */
    while (usbio_bulk(&d->io, s->ep, scratch, sizeof(scratch), 60u,
                      &got) == 0 && got > 0)
        ;
    /* selene's capture path comes up with START; dione's with the first
     * STREAM frame. */
    if (d->model->gen == MB8_GEN_SELENE) {
        rc = mb8_start(d);
        if (rc != MB8_OK)
            goto fail;
    }
    if (mb8_thread_start(&s->reader, reader_main, d) != 0) {
        rc = mb8__fail(d, MB8_E_NOMEM, "cannot start the reader thread");
        goto fail;
    }
    if (mb8_thread_start(&s->pump, pump_main, d) != 0) {
        s->quit = 2;
        mb8_thread_join(s->reader);
        rc = mb8__fail(d, MB8_E_NOMEM, "cannot start the pump thread");
        goto fail;
    }
    s->running = 1;
    return MB8_OK;

fail:
    free(s->ring.buf);
    free(s->work);
    s->ring.buf = NULL;
    s->work = NULL;
    return rc;
}

/* STOP, then let the reader take what is still in flight. */
int mb8_stream_stop(mb8_dev *d)
{
    struct mb8_stream *s = &d->s;
    int rc;

    if (!s->running)
        return MB8_OK;
    s->quit = 1;
    mb8_thread_join(s->pump);
    rc = mb8_stop(d);
    mb8_sleep_ms(250u);
    s->quit = 2;
    mb8_thread_join(s->reader);
    s->running = 0;
    mb8_mutex_lock(&s->lock);
    free(s->ring.buf);
    s->ring.buf = NULL;
    s->ring.cap = s->ring.len = s->ring.head = 0;
    mb8_mutex_unlock(&s->lock);
    free(s->work);
    s->work = NULL;
    return rc;
}

int mb8_stream_running(const mb8_dev *d) { return d->s.running; }

long mb8_stream_read(mb8_dev *d, uint8_t *buf, size_t cap,
                     unsigned timeout_ms)
{
    struct mb8_stream *s = &d->s;
    uint64_t end = mb8_now_ms() + timeout_ms;
    size_t n;

    if (!s->running)
        return mb8__fail(d, MB8_E_STATE, "the stream is not running");
    if (cap < TS_PKT)
        return mb8__fail(d, MB8_E_INVAL, "buffer smaller than a packet");
    mb8_mutex_lock(&s->lock);
    while (s->ring.len < TS_PKT && !s->failed) {
        uint64_t now = mb8_now_ms();

        if (now >= end)
            break;
        mb8_cond_wait_ms(&s->more, &s->lock, (unsigned)(end - now));
    }
    if (s->ring.len < TS_PKT && s->failed) {
        mb8__fail(d, MB8_E_IO, "%s", s->err);
        mb8_mutex_unlock(&s->lock);
        return MB8_E_IO;
    }
    n = mb8__ring_get(&s->ring, buf, cap);
    mb8_mutex_unlock(&s->lock);
    return (long)n;
}

size_t mb8_stream_ready(mb8_dev *d)
{
    size_t n;

    mb8_mutex_lock(&d->s.lock);
    n = d->s.ring.len;
    mb8_mutex_unlock(&d->s.lock);
    return n;
}

void mb8_stream_purge(mb8_dev *d)
{
    mb8_mutex_lock(&d->s.lock);
    d->s.ring.head = d->s.ring.len = 0;
    mb8_mutex_unlock(&d->s.lock);
}

void mb8_stream_stats(mb8_dev *d, struct mb8_stream_stats *st)
{
    mb8_mutex_lock(&d->s.lock);
    *st = d->s.st;
    mb8_mutex_unlock(&d->s.lock);
}
