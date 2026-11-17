/* card.c -- see mb8card.h.  The frames are MB8AC031-CFW's band
 * 0x50..0x57 (its docs/card.md).
 *
 * SPDX-License-Identifier: MIT
 *
 * The firmware has no clock, so every wait is the host's: CARD_ON and
 * CARD_RECV answer FW_EAGAIN until they are done, CARD_ON saying how many
 * ms to wait before asking again.
 */
#include <stdlib.h>
#include <string.h>

#include "devpriv.h"
#include "frq1.h"
#include "mb8cfw/mb8card.h"
#include "t1.h"

/* Report offsets inside the data field. */
#define RP_STAGE        0x01u
#define RP_WAIT         0x02u
#define RP_ATRLEN       0x03u
#define RP_EVENT        0x04u
#define RP_ATR          0x10u
#define RP_ATR_INLINE   24u
#define RV_GOT          0x02u
#define RV_NEED         0x04u
#define RV_BYTES        0x08u
#define RV_INLINE       32u

struct mb8_card {
    mb8_dev *d;
    struct atr_info ai;
    struct t1 t1;
    int powered;
};

static unsigned get16(const uint8_t *b, unsigned off)
{
    return ((unsigned)b[off] << 8) | b[off + 1];
}

int mb8_card_new(mb8_card **out, mb8_dev *d)
{
    mb8_card *c;

    *out = NULL;
    if (!(d->model->can & MB8_CAN_CARD))
        return mb8__fail(d, MB8_E_UNSUPP, "%s: the card frames have not "
                         "been run on this model", d->model->name);
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return MB8_E_NOMEM;
    c->d = d;
    *out = c;
    return MB8_OK;
}

void mb8_card_free(mb8_card *c) { free(c); }

void mb8_card_rebind(mb8_card *c, mb8_dev *d)
{
    c->d = d;
    c->t1.ctx = c;
}

int mb8_card_powered(const mb8_card *c) { return c->powered; }

int mb8_card_status(mb8_card *c, unsigned *stage, uint32_t *event,
                    int *present)
{
    uint8_t r[FRQ1_DATA_BYTES];
    int rc = mb8_call(c->d, FRQ1_CMD_CARD_STATUS, 0, 0, NULL, 0, r);
    uint32_t ev;

    if (rc != MB8_OK)
        return rc;
    ev = frq1_get32(r, RP_EVENT);
    if (stage)
        *stage = r[RP_STAGE];
    if (event)
        *event = ev;
    if (present)
        *present = (ev & MB8_CARD_EVENT_PRESENT) != 0;
    return MB8_OK;
}

int mb8_card_power_off(mb8_card *c)
{
    c->powered = 0;
    return mb8_call(c->d, FRQ1_CMD_CARD_OFF, 0, 0, NULL, 0, NULL);
}

/* t1_xfer_fn: PUT the block 40 bytes at a time, SEND, RECV until a whole
 * block is in, GET whatever did not fit the RECV report. */
static int xfer(void *ctx, const uint8_t *tx, size_t ntx, uint8_t *rx,
                size_t *nrx, unsigned wait_ms)
{
    mb8_card *c = ctx;
    mb8_dev *d = c->d;
    uint8_t r[FRQ1_DATA_BYTES];
    uint64_t end;
    size_t off, got, need;
    int rc;

    *nrx = 0;
    for (off = 0; off < ntx; off += FRQ1_DATA_BYTES) {
        size_t k = ntx - off < FRQ1_DATA_BYTES ? ntx - off : FRQ1_DATA_BYTES;

        if (mb8_call(d, FRQ1_CMD_CARD_PUT, (uint32_t)off, (uint32_t)k,
                     tx + off, k, NULL) != MB8_OK)
            return T1_XFER_IO;
    }
    if (mb8_call(d, FRQ1_CMD_CARD_SEND, (uint32_t)ntx, 0, NULL, 0, NULL) !=
        MB8_OK)
        return T1_XFER_IO;
    end = mb8_now_ms() + wait_ms;
    for (;;) {
        rc = mb8_call(d, FRQ1_CMD_CARD_RECV, 0, 0, NULL, 0, r);
        if (rc == MB8_OK)
            break;
        if (rc == MB8_E_IO)             /* more than one block came */
            return T1_XFER_TIMEOUT;
        if (rc != MB8_E_AGAIN)
            return T1_XFER_IO;
        if (mb8_now_ms() > end)
            return T1_XFER_TIMEOUT;
        mb8_sleep_ms(2u);
    }
    got = get16(r, RV_GOT);
    need = get16(r, RV_NEED);
    if (need != got || got > T1_BLOCK_MAX || got < 4u)
        return T1_XFER_TIMEOUT;
    memcpy(rx, r + RV_BYTES, got < RV_INLINE ? got : RV_INLINE);
    for (off = RV_INLINE; off < got; off += FRQ1_DATA_BYTES) {
        size_t k = got - off < FRQ1_DATA_BYTES ? got - off : FRQ1_DATA_BYTES;

        if (mb8_call(d, FRQ1_CMD_CARD_GET, (uint32_t)off, 0, NULL, 0, r) !=
            MB8_OK)
            return T1_XFER_IO;
        memcpy(rx + off, r, k);
    }
    *nrx = got;
    return T1_XFER_OK;
}

static int card_on(mb8_card *c, uint8_t *atr, size_t *natr, unsigned limit)
{
    mb8_dev *d = c->d;
    uint8_t r[FRQ1_DATA_BYTES];
    uint64_t end = mb8_now_ms() + limit;
    size_t n, i;
    int rc;

    *natr = 0;
    for (;;) {
        rc = mb8_call(d, FRQ1_CMD_CARD_ON, 0, 0, NULL, 0, r);
        if (rc != MB8_E_AGAIN)
            break;
        if (mb8_now_ms() > end)
            return mb8__fail(d, MB8_E_TIMEOUT, "no ATR within %u ms (stage "
                             "%u)", limit, r[RP_STAGE]);
        mb8_sleep_ms(r[RP_WAIT]);
    }
    if (rc != MB8_OK)
        return rc;
    n = r[RP_ATRLEN];
    if (n == 0 || n > MB8_ATR_MAX)
        return mb8__fail(d, MB8_E_IO, "CARD_ON: ATR length %u", (unsigned)n);
    for (i = 0; i < n && i < RP_ATR_INLINE; i++)
        atr[i] = r[RP_ATR + i];
    if (n > RP_ATR_INLINE) {
        rc = mb8_call(d, FRQ1_CMD_CARD_GET, RP_ATR_INLINE, 0, NULL, 0, r);
        if (rc != MB8_OK)
            return rc;
        memcpy(atr + RP_ATR_INLINE, r, n - RP_ATR_INLINE);
    }
    *natr = n;
    return MB8_OK;
}

int mb8_card_power_on(mb8_card *c, uint8_t atr[MB8_ATR_MAX], size_t *natr,
                      unsigned limit_ms)
{
    mb8_dev *d = c->d;
    const char *why;
    int rc;

    c->powered = 0;
    *natr = 0;
    rc = mb8_card_power_off(c);
    if (rc != MB8_OK)
        return rc;
    rc = card_on(c, atr, natr, limit_ms ? limit_ms : 3000u);
    if (rc != MB8_OK)
        return rc;
    why = atr_parse(atr, *natr, &c->ai);
    if (why != NULL) {
        (void)mb8_card_power_off(c);
        return mb8__fail(d, MB8_E_IO, "ATR: %s", why);
    }
    if (!(c->ai.protocols & 2u) || c->ai.crc) {
        (void)mb8_card_power_off(c);
        return mb8__fail(d, MB8_E_UNSUPP, "the card offers no T=1 with LRC");
    }
    rc = mb8_call(d, FRQ1_CMD_CARD_PARAM, (c->ai.fi << 16) | c->ai.di,
                  (c->ai.cwi << 8) | c->ai.bwi, NULL, 0, NULL);
    if (rc != MB8_OK) {
        (void)mb8_card_power_off(c);
        return rc;
    }
    t1_init(&c->t1, xfer, c, &c->ai, MB8_CARD_IFSD);
    if (t1_set_ifsd(&c->t1) != T1_XFER_OK) {
        (void)mb8_card_power_off(c);
        return mb8__fail(d, MB8_E_IO, "S(IFS %u): %s", MB8_CARD_IFSD,
                         c->t1.err);
    }
    c->powered = 1;
    return MB8_OK;
}

int mb8_card_transmit(mb8_card *c, const uint8_t *apdu, size_t napdu,
                      uint8_t *resp, size_t cap, size_t *nresp)
{
    *nresp = 0;
    if (!c->powered)
        return mb8__fail(c->d, MB8_E_STATE, "the card is not powered");
    if (t1_transceive(&c->t1, apdu, napdu, resp, cap, nresp) != 0)
        return mb8__fail(c->d, MB8_E_IO, "T=1: %s", c->t1.err);
    return MB8_OK;
}

/* --- presence ---------------------------------------------------------- */

static int held(const struct mb8_presence *p)
{
    return p->powered || p->known_present;
}

enum mb8_presence_step mb8_presence_plan(struct mb8_presence *p,
                                         uint64_t now)
{
    if (held(p) && !p->removable)
        return MB8_PRESENCE_YES;
    if (now < p->next_probe)
        return held(p) ? MB8_PRESENCE_YES : MB8_PRESENCE_NO;
    p->next_probe = now + (p->removable ? MB8_PRESENCE_REMOVABLE_MS
                                        : MB8_PRESENCE_EVERY_MS);
    return MB8_PRESENCE_ASK;
}

enum mb8_presence_step mb8_presence_learn(struct mb8_presence *p,
                                          int answered, int present)
{
    if (!answered)
        return held(p) ? MB8_PRESENCE_YES : MB8_PRESENCE_NO;
    if (present) {
        p->known_present = 1;
        return MB8_PRESENCE_YES;
    }
    p->known_present = 0;
    p->powered = 0;
    return MB8_PRESENCE_NO;
}
