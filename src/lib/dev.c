/* dev.c -- models, opening, one frame, identify, start/stop, I2C, tuning.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devpriv.h"
#include "fe_dt295.h"
#include "fe_m2tv.h"
#include "frq1.h"

/* --- models -------------------------------------------------------------
 *
 * Only what has been run with CFW on a real device goes into `can`.  The
 * card bit means the card worked with CFW's own sample host code; through
 * this library it is not confirmed yet (TODO.md, section 0).
 *
 *   m2tv   GV-M2TV, dione.  STREAM measured through this library.
 *          Terrestrial tuning is built in (fe_m2tv.c, measured write by
 *          write).
 *   dt295  PIX-DT295, selene.  STREAM measured through this library.
 *          Terrestrial and satellite tuning are built in (fe_dt295.c,
 *          measured write by write).
 *
 * Other models are left out until CFW runs on them.
 */
static const struct mb8_model MODELS[] = {
    { "m2tv", "GV-M2TV", 0x04BB, 0x053A, MB8_GEN_DIONE, 0x000E9F80u,
      { 0x000C5000u, 0x000C5040u, 0x000C5080u, 0x000C50C0u },
      MB8_CAN_STREAM | MB8_CAN_TUNE | MB8_CAN_TUNE_BUILTIN | MB8_CAN_CARD },
    { "dt295", "PIX-DT295", 0x06B8, 0x103B, MB8_GEN_SELENE, 0x000EA6FCu,
      { 0x003B0000u, 0x003B0040u, 0x003B0080u, 0x003B00C0u },
      MB8_CAN_STREAM | MB8_CAN_TUNE | MB8_CAN_TUNE_BUILTIN | MB8_CAN_SAT |
      MB8_CAN_CARD },
};
#define NMODELS (sizeof(MODELS) / sizeof(MODELS[0]))

const struct mb8_model *mb8_models(size_t *count)
{
    if (count)
        *count = NMODELS;
    return MODELS;
}

const struct mb8_model *mb8_model_find(const char *name)
{
    unsigned vid, pid;
    size_t i;

    if (name == NULL)
        return NULL;
    for (i = 0; i < NMODELS; i++)
        if (strcmp(MODELS[i].name, name) == 0)
            return &MODELS[i];
    if (sscanf(name, "%x:%x", &vid, &pid) == 2)
        for (i = 0; i < NMODELS; i++)
            if (MODELS[i].vid == vid && MODELS[i].pid == pid)
                return &MODELS[i];
    return NULL;
}

const struct mb8_model *mb8_model_of(const mb8_dev *d) { return d->model; }

/* --- errors ------------------------------------------------------------ */

const char *mb8_strerror(int rc)
{
    switch (rc) {
    case MB8_OK:        return "ok";
    case MB8_E_NOTIMPL: return "not implemented in the firmware";
    case MB8_E_IO:      return "the hardware did not answer";
    case MB8_E_TIMEOUT: return "timed out";
    case MB8_E_INVAL:   return "invalid argument";
    case MB8_E_AGAIN:   return "not ready yet";
    case MB8_E_BADCMD:  return "the firmware does not know the command";
    case MB8_E_BUSY:    return "busy";
    case MB8_E_USB:     return "USB transfer failed";
    case MB8_E_SILENT:  return "CFW did not answer";
    case MB8_E_NOCFW:   return "CFW is not installed";
    case MB8_E_NODEV:   return "no device";
    case MB8_E_NOMEM:   return "out of memory";
    case MB8_E_UNSUPP:  return "not supported on this model";
    case MB8_E_FILE:    return "bad file";
    case MB8_E_STATE:   return "wrong state";
    default:            return "unknown error";
    }
}

int mb8__fail(mb8_dev *d, int rc, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(d->err, sizeof(d->err), fmt, ap);
    va_end(ap);
    return rc;
}

int mb8__status(int32_t st)
{
    if (st == FRQ1_ST_OK)
        return MB8_OK;
    if (st >= FRQ1_E_BUSY && st <= FRQ1_E_NOTIMPL)
        return (int)st;         /* the same numbers, by design */
    return MB8_E_IO;
}

uint64_t mb8_clock_ms(void) { return mb8_now_ms(); }

const char *mb8_error(const mb8_dev *d) { return d ? d->err : ""; }

static char g_open_err[288];
const char *mb8_open_error(void) { return g_open_err; }

/* --- open / close ------------------------------------------------------ */

int mb8_open(mb8_dev **out, const struct mb8_open_opts *o)
{
    struct mb8_open_opts dflt = { NULL, MB8_BOX_MAIN, 0 };
    const struct mb8_model *want = NULL;
    mb8_dev *d;
    size_t i;

    *out = NULL;
    if (o == NULL)
        o = &dflt;
    if (o->model != NULL) {
        want = mb8_model_find(o->model);
        if (want == NULL) {
            snprintf(g_open_err, sizeof(g_open_err), "unknown model '%s'",
                     o->model);
            return MB8_E_INVAL;
        }
    }
    if (o->mailbox > 3u) {
        snprintf(g_open_err, sizeof(g_open_err), "mailbox %u", o->mailbox);
        return MB8_E_INVAL;
    }
    d = calloc(1, sizeof(*d));
    if (d == NULL)
        return MB8_E_NOMEM;
    snprintf(g_open_err, sizeof(g_open_err), "no supported device found");
    for (i = 0; i < NMODELS; i++) {
        const struct mb8_model *m = &MODELS[i];

        if (want != NULL && m != want)
            continue;
        if (usbio_open(&d->io, m->vid, m->pid, m->gen == MB8_GEN_DIONE,
                       o->index) == 0) {
            d->model = m;
            break;
        }
        snprintf(g_open_err, sizeof(g_open_err), "%s: %s", m->name,
                 d->io.err);
    }
    if (d->model == NULL) {
        free(d);
        return MB8_E_NODEV;
    }
    d->box = d->model->mbox[o->mailbox];
    if (d->box == 0) {
        snprintf(g_open_err, sizeof(g_open_err), "%s has no confirmed "
                 "mailbox %u", d->model->name, o->mailbox);
        usbio_close(&d->io);
        free(d);
        return MB8_E_UNSUPP;
    }
    d->seq = (uint32_t)mb8_now_ms() * 2654435761u;
    mb8_mutex_init(&d->call_lock);
    mb8_mutex_init(&d->s.lock);
    mb8_cond_init(&d->s.more);
    *out = d;
    return MB8_OK;
}

void mb8_close(mb8_dev *d)
{
    if (d == NULL)
        return;
    if (d->s.running)
        (void)mb8_stream_stop(d);
    usbio_close(&d->io);
    mb8_cond_destroy(&d->s.more);
    mb8_mutex_destroy(&d->s.lock);
    mb8_mutex_destroy(&d->call_lock);
    free(d);
}

/* --- the hook -------------------------------------------------------------
 *
 * Every command starts with a write into a mailbox.  Without CFW that
 * address belongs to the vendor loader, so the first thing is a read: the
 * stub `ldi:20 #entry,r12 ; jmp @r12 ; nop` has six fixed bytes. */
#define MB8_ENTRY 0x000E1000u     /* CFW's frc1_entry, both loaders */

int mb8_hooked(mb8_dev *d)
{
    uint8_t h[8];
    int rc;

    mb8_mutex_lock(&d->call_lock);
    rc = usbio_read(&d->io, d->model->hook, h, sizeof(h));
    mb8_mutex_unlock(&d->call_lock);
    if (rc != 0)
        return mb8__fail(d, MB8_E_USB, "read hook: %s", d->io.err);
    /* The stub (ldi:20 #entry, r12 / jmp @r12 / nop) and an entry that is
     * CFW's: other firmware hooked the same way (one measured jumps
     * to 0x000E14FC) keeps its own data where the mailboxes are. */
    if (h[0] == 0x9Bu && h[1] == (uint8_t)(((MB8_ENTRY >> 16) << 4) | 12u) &&
        h[2] == (uint8_t)(MB8_ENTRY >> 8) && h[3] == (uint8_t)MB8_ENTRY &&
        h[4] == 0x97u && h[5] == 0x0Cu && h[6] == 0x9Fu && h[7] == 0xA0u)
        return 1;
    mb8__fail(d, MB8_E_NOCFW, "CFW is not hooked at 0x%08X",
              (unsigned)d->model->hook);
    return 0;
}

/* --- one frame ------------------------------------------------------------
 *
 * Four checks make a reply (frq1.h): the magic, the command echoed (a
 * read-back with a changed command word is not our firmware), our
 * sequence, and a status that is no longer PENDING (without CFW the
 * read-back is our own request, or zeros on dione). */
int mb8_call(mb8_dev *d, uint32_t cmd, uint32_t arg0, uint32_t arg1,
             const uint8_t *data, size_t ndata, uint8_t reply[40])
{
    uint8_t req[FRQ1_BYTES], rep[FRQ1_BYTES];
    const char *why;
    uint32_t seq;
    int32_t st;

    mb8_mutex_lock(&d->call_lock);
    seq = ++d->seq;
    frq1_build(req, seq, cmd, arg0, arg1, data, ndata);
    if (usbio_dispatch(&d->io, d->box, req, rep) != 0) {
        mb8__fail(d, MB8_E_USB, "%s: %s", frq1_command_name(cmd),
                  d->io.err);
        mb8_mutex_unlock(&d->call_lock);
        return MB8_E_USB;
    }
    mb8_mutex_unlock(&d->call_lock);
    why = frq1_reply_check(rep, seq, cmd);
    if (why != NULL)
        return mb8__fail(d, MB8_E_SILENT, "%s: %s", frq1_command_name(cmd),
                         why);
    if (reply != NULL)
        memcpy(reply, rep + FRQ1_FR_DATA, FRQ1_DATA_BYTES);
    st = frq1_get32s(rep, FRQ1_FR_STATUS);
    if (st != FRQ1_ST_OK)
        mb8__fail(d, mb8__status(st), "%s: %s", frq1_command_name(cmd),
                  frq1_status_name(st));
    return mb8__status(st);
}

int mb8_identify(mb8_dev *d, struct mb8_identity *id)
{
    uint8_t r[FRQ1_DATA_BYTES];
    int rc = mb8_call(d, FRQ1_CMD_IDENTIFY, 0, 0, NULL, 0, r);

    if (rc != MB8_OK)
        return rc;
    memset(id, 0, sizeof(*id));
    id->report_version = r[FRQ1_ID_VERSION];
    id->protocol = r[FRQ1_ID_PROTOCOL];
    id->signature = frq1_get32(r, FRQ1_ID_SIGNATURE);
    if (id->report_version >= 2) {
        id->board = frq1_get32(r, FRQ1_ID_BOARD);
        id->caps = frq1_get32(r, FRQ1_ID_CAPS);
    }
    if (id->report_version >= 3)
        id->build = frq1_get32(r, FRQ1_ID_BUILD);
    if (id->signature != FRQ1_ID_FR01)
        return mb8__fail(d, MB8_E_SILENT, "IDENTIFY: signature 0x%08X is "
                         "not 'FR01'", (unsigned)id->signature);
    return MB8_OK;
}

int mb8_start(mb8_dev *d)
{
    return mb8_call(d, FRQ1_CMD_START, 0, 0, NULL, 0, NULL);
}

int mb8_stop(mb8_dev *d)
{
    return mb8_call(d, FRQ1_CMD_STOP, 0, 0, NULL, 0, NULL);
}

int mb8_i2c(mb8_dev *d, unsigned addr7, const uint8_t *w, unsigned wn,
            uint8_t *r, unsigned rn)
{
    uint8_t rep[FRQ1_DATA_BYTES];
    int rc;

    if (addr7 > 0x7Fu || wn + rn == 0 || wn + rn > FRQ1_I2CX_MAX)
        return mb8__fail(d, MB8_E_INVAL, "I2C: address 0x%X, %u + %u bytes",
                         addr7, wn, rn);
    rc = mb8_call(d, FRQ1_CMD_I2CXFER, FRQ1_I2CX_ARG(addr7, wn, rn), 0, w,
                  wn, rep);
    if (rc == MB8_OK && rn)
        memcpy(r, rep, rn);
    return rc;
}

/* --- tuning ------------------------------------------------------------ */

static int fe_xfer(void *ctx, unsigned addr7, const uint8_t *w, unsigned wn,
                   uint8_t *r, unsigned rn)
{
    return mb8_i2c((mb8_dev *)ctx, addr7, w, wn, r, rn) == MB8_OK ? 0 : -1;
}

/* Prefix the last error with `what` (d->err cannot be both the source and
 * the destination of one snprintf). */
static int wrap(mb8_dev *d, int rc, const char *what)
{
    char last[sizeof(d->err)];

    memcpy(last, d->err, sizeof(last));
    return mb8__fail(d, rc, "%s: %s", what, last);
}

/* The model's front end: fe_m2tv.c or fe_dt295.c. */
static int is_m2tv(const mb8_dev *d)
{
    return d->model->gen == MB8_GEN_DIONE;
}

static int fe_init(mb8_dev *d)
{
    return is_m2tv(d) ? m2tv_init(fe_xfer, d) : dt295_init(fe_xfer, d);
}

static int fe_status(mb8_dev *d, int *locked, uint32_t *cnr)
{
    if (is_m2tv(d))
        return m2tv_status(fe_xfer, d, locked, cnr);
    return (d->fe_sat ? dt295_sat_status : dt295_status)(fe_xfer, d, locked,
                                                         cnr);
}

int mb8_tune_khz(mb8_dev *d, unsigned khz, unsigned wait_ms)
{
    unsigned lo = is_m2tv(d) ? M2TV_KHZ_MIN : DT295_KHZ_MIN;
    unsigned hi = is_m2tv(d) ? M2TV_KHZ_MAX : DT295_KHZ_MAX;
    uint64_t end;
    int locked;

    if (!(d->model->can & MB8_CAN_TUNE))
        return mb8__fail(d, MB8_E_UNSUPP, "%s: the host has no tuning "
                         "sequence for this model", d->model->name);
    if (khz < lo || khz > hi)
        return mb8__fail(d, MB8_E_UNSUPP, "%u kHz: only %u..%u kHz is "
                         "measured on this model", khz, lo, hi);
    if (!d->tuner_inited) {
        if (fe_init(d) != 0)
            return wrap(d, MB8_E_IO, "front end init");
        d->tuner_inited = 1;
    }
    d->fe_sat = 0;
    d->sat_tsid = 0;
    if ((is_m2tv(d) ? m2tv_tune(fe_xfer, d, khz)
                    : dt295_tune(fe_xfer, d, khz)) != 0) {
        d->tuner_inited = 0;
        return wrap(d, MB8_E_IO, "tune");
    }
    end = mb8_now_ms() + wait_ms;
    for (;;) {
        if (fe_status(d, &locked, NULL) != 0)
            return wrap(d, MB8_E_IO, "lock");
        if (locked)
            return 1;
        if (mb8_now_ms() >= end)
            return 0;
        mb8_sleep_ms(20u);
    }
}

int mb8_tune_sat(mb8_dev *d, unsigned if_khz, unsigned ts, unsigned wait_ms)
{
    uint64_t end;
    unsigned picked = 0;
    int locked;

    if (!(d->model->can & MB8_CAN_SAT))
        return mb8__fail(d, MB8_E_UNSUPP, "%s: no satellite tuning on this "
                         "model", d->model->name);
    if (if_khz < DT295_SAT_KHZ_MIN || if_khz > DT295_SAT_KHZ_MAX)
        return mb8__fail(d, MB8_E_UNSUPP, "IF %u kHz: only %u..%u kHz is "
                         "measured on this model", if_khz, DT295_SAT_KHZ_MIN,
                         DT295_SAT_KHZ_MAX);
    if (!d->tuner_inited) {
        if (fe_init(d) != 0)
            return wrap(d, MB8_E_IO, "front end init");
        d->tuner_inited = 1;
    }
    d->fe_sat = 1;
    d->sat_tsid = 0;
    if (dt295_sat_tune(fe_xfer, d, if_khz) != 0) {
        d->tuner_inited = 0;
        return wrap(d, MB8_E_IO, "tune");
    }
    end = mb8_now_ms() + wait_ms;
    for (;;) {
        if (dt295_sat_poll(fe_xfer, d, ts, &picked, &locked) != 0)
            return wrap(d, MB8_E_IO, "lock");
        if (locked) {
            d->sat_tsid = picked;
            return 1;
        }
        if (mb8_now_ms() >= end)
            return 0;
        mb8_sleep_ms(10u);
    }
}

unsigned mb8_sat_tsid(mb8_dev *d)
{
    return d->sat_tsid;
}

/* Lock and C/N: the host's own reading of the demodulator. */
int mb8_signal(mb8_dev *d, struct mb8_signal *s)
{
    uint32_t cnr;
    int locked;

    memset(s, 0, sizeof(*s));
    if (!(d->model->can & MB8_CAN_TUNE))
        return mb8__fail(d, MB8_E_UNSUPP, "%s: no front end on this model",
                         d->model->name);
    if (fe_status(d, &locked, &cnr) != 0)
        return wrap(d, MB8_E_IO, "signal");
    s->locked = locked;
    s->cnr_raw = cnr;
    s->cnr_valid = 1;
    return MB8_OK;
}

/* --- channels ------------------------------------------------------------
 *
 *   "27" / "U27"   UHF 13..62: 473,143 kHz + 6 MHz steps
 *   "C13".."C63"   CATV pass-through.  C13..C22 at 111,143 kHz + 6 MHz
 *                  steps with C22 2 MHz higher (167,143); C23..C63 at
 *                  225,143..465,143 kHz
 *   "557143k"      kHz as is
 * Parsed only; mb8_tune_khz refuses what the model has not been measured
 * on (700 MHz and up, CATV) with MB8_E_UNSUPP. */
unsigned mb8_channel_khz(const char *s)
{
    char *end;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return 0;
    if (s[0] == 'C' || s[0] == 'c') {
        v = strtoul(s + 1, &end, 10);
        if (end == s + 1 || *end || v < 13u || v > 63u)
            return 0;
        if (v <= 22u)
            return 111143u + 6000u * ((unsigned)v - 13u) +
                   (v == 22u ? 2000u : 0u);
        return 225143u + 6000u * ((unsigned)v - 23u);
    }
    if (s[0] == 'U' || s[0] == 'u')
        s++;
    v = strtoul(s, &end, 10);
    if (end == s)
        return 0;
    if ((*end == 'k' || *end == 'K') && end[1] == '\0')
        return (v >= 90000u && v <= 770000u) ? (unsigned)v : 0u;
    if (*end || v < 13u || v > 62u)
        return 0;
    return 473143u + 6000u * ((unsigned)v - 13u);
}

/*   "BS1".."BS23"  odd: IF 1,049,480 kHz + 38,360 kHz per two
 *   "CS2".."CS24"  even: IF 1,613,000 kHz + 40,000 kHz per two
 * followed by "/TS<n>" (n-th TS, 0..7) or "/0x<TSID>" for BS. */
int mb8_channel_parse(const char *s, struct mb8_channel *c)
{
    char *end;
    unsigned long v, t = 0;
    int bs;

    memset(c, 0, sizeof(*c));
    if (s == NULL)
        return MB8_E_INVAL;
    if ((s[0] == 'B' || s[0] == 'b' || s[0] == 'C' || s[0] == 'c') &&
        (s[1] == 'S' || s[1] == 's')) {
        bs = s[0] == 'B' || s[0] == 'b';
        v = strtoul(s + 2, &end, 10);
        if (end == s + 2)
            return MB8_E_INVAL;
        if (*end == '/') {
            const char *p = end + 1;
            if ((p[0] == 'T' || p[0] == 't') && (p[1] == 'S' || p[1] == 's')) {
                t = strtoul(p + 2, &end, 10);
                if (end == p + 2 || t > 7u)
                    return MB8_E_INVAL;
            } else {
                t = strtoul(p, &end, 0);
                if (end == p || t < 8u || t > 0xFFFFu)
                    return MB8_E_INVAL;
            }
        }
        if (*end)
            return MB8_E_INVAL;
        if (bs) {
            if (v < 1u || v > 23u || !(v & 1u))
                return MB8_E_INVAL;
            c->khz = 1049480u + 38360u * ((unsigned)v - 1u) / 2u;
        } else {
            if (v < 2u || v > 24u || (v & 1u))
                return MB8_E_INVAL;
            c->khz = 1613000u + 40000u * ((unsigned)v - 2u) / 2u;
        }
        c->sat = 1;
        c->ts = (unsigned)t;
        return MB8_OK;
    }
    c->khz = mb8_channel_khz(s);
    return c->khz ? MB8_OK : MB8_E_INVAL;
}

int mb8_tune_channel(mb8_dev *d, const struct mb8_channel *c,
                     unsigned wait_ms)
{
    return c->sat ? mb8_tune_sat(d, c->khz, c->ts, wait_ms)
                  : mb8_tune_khz(d, c->khz, wait_ms);
}
