/* usbio.h -- the chip's USB window (vendor request 0xBC) and the bulk
 * endpoint, over libusb-1.0.
 *
 * SPDX-License-Identifier: MIT
 *
 * THE WINDOW.  wValue = (addr >> 16 & 0xFF) << 8 | sub-opcode,
 * wIndex = addr & 0xFFFF: a 24-bit address.  Sub-opcode 0 is the plain
 * window, 0xC1 the doorbell CFW answers.
 *
 * THE LIMITS ARE MEASURED, NOT TASTE (MB8AC031-CFW docs):
 *   - reads at most 32 bytes a transfer (longer register reads have hung a
 *     device until it was replugged)
 *   - selene: block writes of up to 64 bytes, 4-byte aligned
 *   - dione: one aligned word per transfer, always (its loader scatters a
 *     longer OUT and half-writes)
 *   - every control transfer is retried with a backoff: these devices
 *     return LIBUSB_ERROR_NOT_FOUND now and then
 */
#ifndef MB8_USBIO_H
#define MB8_USBIO_H

#include <stdint.h>

#define USBIO_REQ           0xBCu
#define USBIO_READ_MAX      32u
#define USBIO_BLOCK_MAX     64u
#define USBIO_TRIES         6

#define USBIO_SUBOP_PLAIN    0x00u
#define USBIO_SUBOP_DISPATCH 0xC1u

struct usbio {
    void *ctx;                  /* libusb_context *                       */
    void *handle;               /* libusb_device_handle *                 */
    unsigned vid, pid;
    int dione;                  /* word-per-transfer rules                */
    int claimed;
    char err[224];
};

/* Open the `index`-th device with this id.  0, or -1 with err set. */
int  usbio_open(struct usbio *u, unsigned vid, unsigned pid, int dione,
                int index);
void usbio_close(struct usbio *u);
int  usbio_claim(struct usbio *u);
/* Like usbio_claim, but tells "another process holds it" apart:
 * 0 claimed (or already ours), 1 held elsewhere, -1 failed (err set). */
int  usbio_try_claim(struct usbio *u);

int usbio_read(struct usbio *u, uint32_t addr, uint8_t *buf, uint32_t n);
int usbio_write(struct usbio *u, uint32_t addr, const uint8_t *buf,
                uint32_t n);

/* The eight hook bytes in the one order that keeps the chip out of a
 * half-written dispatcher (dione).  `restoring` = putting the vendor's
 * bytes back. */
int usbio_hook_write(struct usbio *u, uint32_t addr, const uint8_t *b,
                     int restoring);

/* A frame into the mailbox at `box`, the doorbell, the answer read back. */
int usbio_dispatch(struct usbio *u, uint32_t box, const uint8_t out[64],
                   uint8_t in[64]);

/* One bulk read.  0 with *got bytes (maybe after a timeout), 1 nothing
 * arrived in `ms`, -1 failed. */
int usbio_bulk(struct usbio *u, unsigned ep, uint8_t *buf, int len,
               unsigned ms, int *got);

#endif
