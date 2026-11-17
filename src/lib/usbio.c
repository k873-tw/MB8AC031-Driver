/* usbio.c -- see usbio.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include <libusb.h>

#include "plat.h"
#include "usbio.h"

#define H(u) ((libusb_device_handle *)(u)->handle)

int usbio_open(struct usbio *u, unsigned vid, unsigned pid, int dione,
               int index)
{
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    libusb_device_handle *h = NULL;
    ssize_t n, i;
    int seen = 0, rc, last = 0;

    memset(u, 0, sizeof(*u));
    u->vid = vid;
    u->pid = pid;
    u->dione = dione;

    rc = libusb_init(&ctx);
    if (rc != 0) {
        snprintf(u->err, sizeof(u->err), "libusb_init: %s",
                 libusb_error_name(rc));
        return -1;
    }
    n = libusb_get_device_list(ctx, &list);
    for (i = 0; i < n && h == NULL; i++) {
        struct libusb_device_descriptor dd;

        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;
        if (dd.idVendor != vid || dd.idProduct != pid)
            continue;
        if (seen++ != index)
            continue;
        last = libusb_open(list[i], &h);
        if (last != 0)
            h = NULL;
    }
    if (list != NULL)
        libusb_free_device_list(list, 1);
    if (h == NULL) {
        if (seen > index)
            snprintf(u->err, sizeof(u->err), "%04x:%04x #%d: %s", vid, pid,
                     index, last == LIBUSB_ERROR_ACCESS
                     ? "no permission (udev rule?)"
                     : last == LIBUSB_ERROR_NOT_SUPPORTED
                     ? "no WinUSB driver bound (see docs/windows.md)"
                     : libusb_error_name(last));
        else
            snprintf(u->err, sizeof(u->err), "%04x:%04x #%d is not "
                     "connected", vid, pid, index);
        libusb_exit(ctx);
        return -1;
    }
    u->ctx = ctx;
    u->handle = h;
    return 0;
}

void usbio_close(struct usbio *u)
{
    if (u->handle != NULL) {
        if (u->claimed)
            (void)libusb_release_interface(H(u), 0);
        libusb_close(H(u));
        u->handle = NULL;
    }
    if (u->ctx != NULL) {
        libusb_exit((libusb_context *)u->ctx);
        u->ctx = NULL;
    }
    u->claimed = 0;
}

/* Bulk needs the interface; EP0 does not.  So the card driver, which only
 * uses EP0, never takes the interface away from a recorder.
 *
 * The interface is also the one lock between processes: a claim is
 * exclusive across processes, and a claim is kept until the handle is
 * closed.  Linux reports someone else's claim as BUSY, macOS as ACCESS
 * (exclusive access). */
int usbio_try_claim(struct usbio *u)
{
    int rc;

    if (u->claimed)
        return 0;
    (void)libusb_set_auto_detach_kernel_driver(H(u), 1);
    rc = libusb_claim_interface(H(u), 0);
    if (rc == 0) {
        u->claimed = 1;
        return 0;
    }
    snprintf(u->err, sizeof(u->err), "cannot claim interface 0: %s",
             libusb_error_name(rc));
    return rc == LIBUSB_ERROR_BUSY || rc == LIBUSB_ERROR_ACCESS ? 1 : -1;
}

int usbio_claim(struct usbio *u)
{
    return usbio_try_claim(u) == 0 ? 0 : -1;
}

static int xfer(struct usbio *u, uint8_t dir, uint32_t addr, unsigned subop,
                uint8_t *data, uint16_t len, unsigned timeout)
{
    uint16_t wvalue = (uint16_t)((((addr >> 16) & 0xFFu) << 8) | subop);
    uint16_t windex = (uint16_t)(addr & 0xFFFFu);
    int rc = LIBUSB_ERROR_OTHER;
    int i;

    for (i = 0; i < USBIO_TRIES; i++) {
        rc = libusb_control_transfer(H(u), dir, (uint8_t)USBIO_REQ, wvalue,
                                     windex, data, len, timeout);
        if (rc >= 0 || rc == LIBUSB_ERROR_NO_DEVICE)
            break;
        mb8_sleep_ms(50u * (unsigned)(i + 1));
    }
    if (rc < 0) {
        snprintf(u->err, sizeof(u->err), "%s at 0x%08X (sub-opcode 0x%X): %s",
                 dir == 0xC0 ? "read" : "write", (unsigned)addr, subop,
                 libusb_error_name(rc));
        return -1;
    }
    if ((uint16_t)rc != len) {
        snprintf(u->err, sizeof(u->err), "%s at 0x%08X moved %d of %u bytes",
                 dir == 0xC0 ? "read" : "write", (unsigned)addr, rc,
                 (unsigned)len);
        return -1;
    }
    return 0;
}

int usbio_read(struct usbio *u, uint32_t addr, uint8_t *buf, uint32_t n)
{
    uint32_t done = 0;

    if (u->dione) {
        /* The dione loader always moves whole words from the word that
         * holds the address: read that and trim. */
        if ((addr & 3u) != 0u) {
            uint8_t tmp[4];
            uint32_t skew = addr & 3u, take = 4u - skew;

            if (take > n)
                take = n;
            if (xfer(u, 0xC0, addr - skew, USBIO_SUBOP_PLAIN, tmp, 4u,
                     800u) != 0)
                return -1;
            memcpy(buf, tmp + skew, take);
            done = take;
        }
        while (done < n) {
            uint32_t left = n - done;

            if (left >= 4u) {
                uint32_t k = left < USBIO_READ_MAX ? left & ~3u
                                                   : USBIO_READ_MAX;

                if (xfer(u, 0xC0, addr + done, USBIO_SUBOP_PLAIN, buf + done,
                         (uint16_t)k, 800u) != 0)
                    return -1;
                done += k;
            } else {
                uint8_t tmp[4];

                if (xfer(u, 0xC0, addr + done, USBIO_SUBOP_PLAIN, tmp, 4u,
                         800u) != 0)
                    return -1;
                memcpy(buf + done, tmp, left);
                done += left;
            }
        }
        return 0;
    }
    while (done < n) {
        uint32_t k = n - done < USBIO_READ_MAX ? n - done : USBIO_READ_MAX;

        if (xfer(u, 0xC0, addr + done, USBIO_SUBOP_PLAIN, buf + done,
                 (uint16_t)k, 800u) != 0)
            return -1;
        done += k;
    }
    return 0;
}

int usbio_write(struct usbio *u, uint32_t addr, const uint8_t *buf,
                uint32_t n)
{
    uint8_t tmp[USBIO_BLOCK_MAX];
    uint32_t done = 0;

    if (u->dione) {
        /* One word a transfer.  A partial word is merged with what is
         * there, never zero-filled. */
        while (done < n) {
            uint32_t here = addr + done, off = here & 3u, left = n - done;
            uint32_t take = 4u - off;

            if (take > left)
                take = left;
            if (off == 0u && take == 4u) {
                memcpy(tmp, buf + done, 4);
            } else {
                if (usbio_read(u, here - off, tmp, 4u) != 0)
                    return -1;
                memcpy(tmp + off, buf + done, take);
            }
            if (xfer(u, 0x40, here - off, USBIO_SUBOP_PLAIN, tmp, 4u,
                     1000u) != 0)
                return -1;
            done += take;
        }
        return 0;
    }
    while (done < n) {
        uint32_t left = n - done, here = addr + done, k;

        if ((here & 3u) == 0u && left >= 4u) {
            k = left < USBIO_BLOCK_MAX ? left & ~3u : USBIO_BLOCK_MAX;
            memcpy(tmp, buf + done, k);
            if (xfer(u, 0x40, here, USBIO_SUBOP_PLAIN, tmp, (uint16_t)k,
                     1000u) != 0)
                return -1;
            done += k;
            continue;
        }
        if ((here & 1u) != 0u || left < 2u) {
            snprintf(u->err, sizeof(u->err), "write at 0x%08X: the window "
                     "writes whole half-words", (unsigned)here);
            return -1;
        }
        tmp[0] = buf[done];
        tmp[1] = buf[done + 1];
        if (xfer(u, 0x40, here, USBIO_SUBOP_PLAIN, tmp, 2u, 800u) != 0)
            return -1;
        done += 2u;
    }
    return 0;
}

/* dione: the two words cannot go in one transfer, and between them the
 * chip runs whatever the eight bytes say.  Installing writes word 0 first
 * (the stub's ldi:20 falls into the vendor's own branch at hook+8);
 * restoring writes word 1 first.  A word whose second byte is 0x01 would
 * send that intermediate down the read-modify-write path, so it is
 * refused. */
int usbio_hook_write(struct usbio *u, uint32_t addr, const uint8_t *b,
                     int restoring)
{
    uint32_t order[2];
    uint8_t w[4];
    int i;

    if (!u->dione)
        return usbio_write(u, addr, b, 8u);
    order[0] = restoring ? 4u : 0u;
    order[1] = restoring ? 0u : 4u;
    for (i = 0; i < 2; i++) {
        uint32_t k = order[i];

        if (b[k + 1u] == 0x01u) {
            snprintf(u->err, sizeof(u->err), "hook word +%u has 0x01 as its "
                     "second byte: refused", (unsigned)k);
            return -1;
        }
        memcpy(w, b + k, 4);
        if (xfer(u, 0x40, addr + k, USBIO_SUBOP_PLAIN, w, 4u, 1000u) != 0)
            return -1;
    }
    return 0;
}

int usbio_dispatch(struct usbio *u, uint32_t box, const uint8_t out[64],
                   uint8_t in[64])
{
    uint8_t door[4] = { 0, 0, 0, 0 };

    /* A mailbox: plain writes, then exactly four bytes of sub-opcode 0xC1
     * aimed at it. */
    if (usbio_write(u, box, out, 64u) != 0)
        return -1;
    if (xfer(u, 0x40, box, USBIO_SUBOP_DISPATCH, door, 4u, 20000u) != 0)
        return -1;
    return usbio_read(u, box, in, 64u);
}

int usbio_bulk(struct usbio *u, unsigned ep, uint8_t *buf, int len,
               unsigned ms, int *got)
{
    int moved = 0, rc;

    *got = 0;
    if (usbio_claim(u) != 0)
        return -1;
    rc = libusb_bulk_transfer(H(u), (unsigned char)ep, buf, len, &moved, ms);
    *got = moved;
    if (rc == 0 || (rc == LIBUSB_ERROR_TIMEOUT && moved > 0))
        return 0;
    if (rc == LIBUSB_ERROR_TIMEOUT)
        return 1;
    snprintf(u->err, sizeof(u->err), "bulk read on ep 0x%02X: %s", ep,
             libusb_error_name(rc));
    return -1;
}
