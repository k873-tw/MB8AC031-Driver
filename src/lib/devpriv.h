/* devpriv.h -- what is inside an mb8_dev.  Library-internal.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MB8_DEVPRIV_H
#define MB8_DEVPRIV_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "mb8cfw/mb8cfw.h"
#include "plat.h"
#include "usbio.h"

struct mb8_ring {               /* whole 188-byte packets for the reader */
    uint8_t *buf;
    size_t cap, head, len;      /* head = oldest byte                    */
};

struct mb8_stream {
    int running;
    volatile int quit;          /* the two threads look at it            */
    int failed;                 /* a thread died; err says why           */
    char err[384];
    mb8_thread pump, reader;
    unsigned bursts, ring_mib;
    unsigned ep;
    struct mb8_ring ring;
    mb8_mutex lock;             /* the ring, the stats, failed/err       */
    mb8_cond more;              /* a packet arrived, or a thread died    */
    /* the reader's scratch and partial packet */
    uint8_t *work;
    uint8_t carry[188];
    size_t ncarry;
    int synced;
    struct mb8_stream_stats st;
};

struct mb8_dev {
    const struct mb8_model *model;
    struct usbio io;
    uint32_t box;               /* this handle's mailbox                 */
    uint32_t seq;
    mb8_mutex call_lock;        /* one frame at a time on this mailbox   */
    char err[288];

    int tuner_inited;
    int fe_sat;                 /* last tune was the satellite side      */
    unsigned sat_tsid;          /* the TSID it locked to                 */

    struct mb8_stream s;
};

int  mb8__fail(mb8_dev *d, int rc, const char *fmt, ...);

/* stream.c, exposed for the tests: raw bytes in, whole packets into
 * s->ring (s->work must hold 65536 + 188 bytes); packets out. */
void   mb8__stream_feed(struct mb8_stream *s, const uint8_t *p, size_t n);
size_t mb8__ring_get(struct mb8_ring *r, uint8_t *p, size_t n);
int  mb8__status(int32_t fw_status);

#endif
