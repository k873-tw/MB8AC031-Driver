/* install.c -- putting an FRC1 container into the chip.
 *
 * SPDX-License-Identifier: MIT
 *
 * The same five steps, in the same order, as MB8AC031-CFW's frc1_load
 * (its docs/frc1.md, the section on the install order).  Every step is there
 * because leaving it out broke something on a real device:
 *
 *   1   state reads 0: no vendor container has booted
 *   1b  the vendor's eight hook bytes: read off the chip (checked against
 *       the container's crc32), or from "<file>.hook" if our hook is in
 *   2   the running loader is the one the container was built against
 *   2b  the same build already in: write nothing
 *   2c  take interface 0 before the first write, and keep it (below)
 *   3   the vendor's bytes go back FIRST (the code a hook of ours points
 *       at is what services this very transfer)
 *   4   the segments, read back
 *   5   the hook LAST, so the entry never points at half-written code
 *
 * Step 2c is what keeps two processes from installing at once, and keeps
 * an install from rewriting the code under another process's stream:
 * EP0 alone is shared, but a claim on interface 0 is exclusive across
 * processes, and a stream holds it for as long as its handle is open.
 * The claim is kept after the install, so the process that installed
 * also gets to stream (releasing it would let a second process slip in
 * between the install and the first STREAM).  Steps 1-2b only read, so a
 * second process whose build is already in never waits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devpriv.h"
#include "frc1.h"

struct ilog { mb8_log_fn fn; void *ctx; };

static void say(struct ilog *l, const char *fmt, ...)
{
    char line[320];
    va_list ap;

    if (l->fn == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    l->fn(l->ctx, line);
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long n;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 &&
        fseek(f, 0, SEEK_SET) == 0) {
        b = malloc((size_t)n);
        if (b != NULL && fread(b, 1, (size_t)n, f) != (size_t)n) {
            free(b);
            b = NULL;
        }
        *len = (size_t)n;
    }
    fclose(f);
    return b;
}

static int side_load(const char *frc, uint8_t b[8], uint32_t want)
{
    char path[1024];
    FILE *f;
    size_t got;

    snprintf(path, sizeof(path), "%s.hook", frc);
    f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    got = fread(b, 1, 8, f);
    fclose(f);
    return got == 8 && frc1_crc32(0, b, 8) == want ? 0 : -1;
}

static void side_save(const char *frc, const uint8_t b[8], struct ilog *l)
{
    char path[1024];
    FILE *f;

    snprintf(path, sizeof(path), "%s.hook", frc);
    f = fopen(path, "wb");
    if (f == NULL || fwrite(b, 1, 8, f) != 8)
        say(l, "   could not write %s (a replug also restores them)", path);
    else
        say(l, "   kept the vendor's bytes in %s", path);
    if (f != NULL)
        fclose(f);
}

static int same_build_in(struct usbio *u, const struct frc1 *c,
                         uint32_t which, const struct frc1_target *t)
{
    uint8_t stub[8], buf[8];
    uint32_t i;

    frc1_hook_stub(t->entry, stub);
    if (usbio_read(u, t->hook, buf, 8) != 0)
        return -1;
    if (memcmp(buf, stub, 8) != 0)
        return 0;
    for (i = 0; i < c->h.segment_count; i++) {
        struct frc1_segment s;
        uint8_t *back;
        int same;

        frc1_segment_at(c, i, &s);
        if (!(s.target_mask & (1u << which)) || !(s.flags & FRC1_SEG_CODE))
            continue;
        back = malloc(s.bytes ? s.bytes : 1);
        if (back == NULL)
            return -1;
        if (usbio_read(u, s.address, back, s.bytes) != 0) {
            free(back);
            return -1;
        }
        same = memcmp(back, frc1_segment_data(c, &s), s.bytes) == 0;
        free(back);
        if (!same)
            return 0;
    }
    return 1;
}

#define INSTALL_WAIT_MS 30000u    /* for another process's install       */

/* Step 2c.  MB8_OK with interface 0 ours, or MB8_E_BUSY when someone else
 * kept it for INSTALL_WAIT_MS. */
static int take_device(mb8_dev *d, struct ilog *l)
{
    struct usbio *u = &d->io;
    uint64_t t0 = mb8_now_ms();
    int rc, said = 0;

    for (;;) {
        rc = usbio_try_claim(u);
        if (rc == 0)
            return MB8_OK;
        if (rc < 0)
            return mb8__fail(d, MB8_E_USB, "%s", u->err);
        if (mb8_now_ms() - t0 >= INSTALL_WAIT_MS)
            return mb8__fail(d, MB8_E_BUSY, "another program has held the "
                             "device for %u s (recording, or installing); "
                             "nothing written", INSTALL_WAIT_MS / 1000u);
        if (!said) {
            say(l, "   another program is using the device; waiting");
            said = 1;
        }
        mb8_sleep_ms(100);
    }
}

static int install_locked(mb8_dev *d, const char *path, const struct frc1 *c,
                          int force, struct ilog *l)
{
    struct usbio *u = &d->io;
    struct frc1_target t;
    uint8_t buf[8], vhook[8], stub[8];
    uint32_t which = ~0u, i;
    int rc;

    for (i = 0; i < c->h.target_count; i++) {
        frc1_target_at(c, i, &t);
        if (t.usb_vendor == d->model->vid && t.usb_product == d->model->pid) {
            which = i;
            break;
        }
    }
    if (which == ~0u)
        return mb8__fail(d, MB8_E_FILE, "%s has no target for %04x:%04x",
                         path, d->model->vid, d->model->pid);
    say(l, "target %u: %s %04x:%04x", which, frc1_string(c, t.name),
        (unsigned)t.usb_vendor, (unsigned)t.usb_product);
    if (t.entry >= 0x100000u)
        return mb8__fail(d, MB8_E_FILE, "entry 0x%08X does not fit the hook",
                         (unsigned)t.entry);

    say(l, "1. state");
    if (usbio_read(u, t.state, buf, 4) != 0)
        return mb8__fail(d, MB8_E_USB, "%s", u->err);
    if (frc1_be32(buf) != 0)
        return mb8__fail(d, MB8_E_STATE, "state is 0x%08X: a vendor "
                         "container has booted; power-cycle the device",
                         (unsigned)frc1_be32(buf));

    say(l, "1b. the vendor's eight bytes");
    if (usbio_read(u, t.hook, vhook, 8) != 0)
        return mb8__fail(d, MB8_E_USB, "%s", u->err);
    if (frc1_crc32(0, vhook, 8) == t.hook_crc32) {
        side_save(path, vhook, l);
    } else if (side_load(path, vhook, t.hook_crc32) == 0) {
        say(l, "   a hook is in place; took them from %s.hook", path);
    } else {
        return mb8__fail(d, MB8_E_STATE, "the hook does not hold the "
                         "vendor's bytes and there is no %s.hook; replug the "
                         "device (the loader is in RAM)", path);
    }

    say(l, "2. the loader");
    if (t.loader_crc32 == 0) {
        if (!force)
            return mb8__fail(d, MB8_E_FILE, "the container does not say "
                             "which loader it was built for; refusing");
    } else {
        uint8_t *image = malloc(t.loader_bytes ? t.loader_bytes : 1);
        uint32_t got;

        if (image == NULL)
            return MB8_E_NOMEM;
        if (usbio_read(u, t.loader_base, image, t.loader_bytes) != 0) {
            free(image);
            return mb8__fail(d, MB8_E_USB, "%s", u->err);
        }
        /* The hook lies inside the loader: hash it with the vendor's bytes
         * in place, whatever is installed now. */
        if (t.hook >= t.loader_base &&
            t.hook + 8 <= t.loader_base + t.loader_bytes)
            memcpy(image + (t.hook - t.loader_base), vhook, 8);
        got = frc1_crc32(0, image, t.loader_bytes);
        free(image);
        if (got != t.loader_crc32)
            return mb8__fail(d, MB8_E_FILE, "the loader is 0x%08X, the "
                             "container was built for 0x%08X",
                             (unsigned)got, (unsigned)t.loader_crc32);
    }

    if (!force) {
        rc = same_build_in(u, c, which, &t);
        if (rc < 0)
            return mb8__fail(d, MB8_E_USB, "%s", u->err);
        if (rc == 1) {
            say(l, "2b. this build is already in; nothing written");
            return MB8_OK;
        }
    }

    say(l, "2c. take the device");
    rc = take_device(d, l);
    if (rc != MB8_OK)
        return rc;
    /* Whoever held it may have installed this very build meanwhile. */
    if (!force) {
        rc = same_build_in(u, c, which, &t);
        if (rc < 0)
            return mb8__fail(d, MB8_E_USB, "%s", u->err);
        if (rc == 1) {
            say(l, "   this build went in meanwhile; nothing written");
            return MB8_OK;
        }
    }

    say(l, "3. the vendor dispatcher goes back first");
    if (usbio_read(u, t.hook, buf, 8) != 0)
        return mb8__fail(d, MB8_E_USB, "%s", u->err);
    if (memcmp(buf, vhook, 8) != 0 &&
        usbio_hook_write(u, t.hook, vhook, 1) != 0)
        return mb8__fail(d, MB8_E_USB, "%s", u->err);

    say(l, "4. segments");
    for (i = 0; i < c->h.segment_count; i++) {
        struct frc1_segment s;
        const uint8_t *data;

        frc1_segment_at(c, i, &s);
        if (!(s.target_mask & (1u << which)))
            continue;
        data = frc1_segment_data(c, &s);
        say(l, "   %-10s 0x%08X %u bytes", frc1_string(c, s.name),
            (unsigned)s.address, (unsigned)s.bytes);
        if (usbio_write(u, s.address, data, s.bytes) != 0)
            return mb8__fail(d, MB8_E_USB, "%s", u->err);
        if (s.flags & FRC1_SEG_VERIFY) {
            uint8_t *back = malloc(s.bytes ? s.bytes : 1);
            int bad;

            if (back == NULL)
                return MB8_E_NOMEM;
            bad = usbio_read(u, s.address, back, s.bytes) != 0 ||
                  memcmp(back, data, s.bytes) != 0;
            free(back);
            if (bad)
                return mb8__fail(d, MB8_E_IO, "segment '%s' read back "
                                 "different", frc1_string(c, s.name));
        }
    }

    say(l, "5. the hook, last");
    frc1_hook_stub(t.entry, stub);
    if (usbio_hook_write(u, t.hook, stub, 0) != 0)
        return mb8__fail(d, MB8_E_USB, "%s", u->err);
    if (usbio_read(u, t.hook, buf, 8) != 0)
        return mb8__fail(d, MB8_E_USB, "%s", u->err);
    say(l, "installed");
    return MB8_OK;
}

int mb8_install(mb8_dev *d, const char *path, int force, mb8_log_fn log,
                void *ctx)
{
    struct ilog l = { log, ctx };
    struct frc1 c;
    char err[256];
    uint8_t *blob;
    size_t len = 0;
    int rc;

    if (d->s.running)
        return mb8__fail(d, MB8_E_STATE, "stop the stream first");
    blob = slurp(path, &len);
    if (blob == NULL)
        return mb8__fail(d, MB8_E_FILE, "%s: cannot read", path);
    if (frc1_open(&c, blob, len, err, sizeof(err)) != 0) {
        free(blob);
        return mb8__fail(d, MB8_E_FILE, "%s: %s", path, err);
    }
    mb8_mutex_lock(&d->call_lock);
    rc = install_locked(d, path, &c, force, &l);
    mb8_mutex_unlock(&d->call_lock);
    free(blob);
    return rc;
}

int mb8_ensure_firmware(mb8_dev *d, const char *path, mb8_log_fn log,
                        void *ctx)
{
    int rc = mb8_hooked(d);

    if (rc == 1)
        return MB8_OK;
    if (rc < 0)
        return rc;
    if (path == NULL || *path == '\0')
        return mb8__fail(d, MB8_E_NOCFW, "CFW is not installed and no "
                         "firmware file was given");
    return mb8_install(d, path, 0, log, ctx);
}
