/* mb8card.h -- the card slot (ISO 7816, T=1) through CFW's card frames.
 *
 * SPDX-License-Identifier: MIT
 *
 * The firmware moves bytes and nothing else: it powers the card and
 * collects the ATR (CARD_ON), sends one block the host built and hands one
 * back (CARD_PUT / SEND / RECV / GET).  T=1 itself -- sequence numbers,
 * chaining, retransmission, waiting time extension, IFS -- is here, on the
 * host.  Only T=1 with LRC is spoken; no PPS (the card runs in the mode its
 * ATR names).
 *
 * Open the device on MB8_BOX_CARD, so a recorder on MB8_BOX_MAIN and the
 * card driver do not overwrite each other's frames.
 */
#ifndef MB8CARD_H
#define MB8CARD_H

#include <stddef.h>
#include <stdint.h>

#include "mb8cfw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB8_ATR_MAX         33u
#define MB8_CARD_IFSD       251u    /* blocks stay under 256 bytes        */
#define MB8_CARD_EVENT_PRESENT 0x400u

typedef struct mb8_card mb8_card;

/* The card sits on an open device; the device must outlive it. */
int  mb8_card_new(mb8_card **out, mb8_dev *d);
void mb8_card_free(mb8_card *c);

/* Move the card's state (T=1 sequence numbers, the ATR's parameters) onto
 * a device handle opened again: a driver that opens the USB device only
 * inside each call keeps one mb8_card across calls. */
void mb8_card_rebind(mb8_card *c, mb8_dev *d);

/* CARD_STATUS: read only.  *present from CARD_EVENT bit 10. */
int mb8_card_status(mb8_card *c, unsigned *stage, uint32_t *event,
                    int *present);

/* Power up, collect the ATR, set the speed and waits it names, and agree
 * IFSD with S(IFS).  `limit_ms` for the ATR (0 = 3000). */
int mb8_card_power_on(mb8_card *c, uint8_t atr[MB8_ATR_MAX], size_t *natr,
                      unsigned limit_ms);
int mb8_card_power_off(mb8_card *c);
int mb8_card_powered(const mb8_card *c);

/* One APDU, one response (data and SW1 SW2). */
int mb8_card_transmit(mb8_card *c, const uint8_t *apdu, size_t napdu,
                      uint8_t *resp, size_t cap, size_t *nresp);

/* --- presence ------------------------------------------------------------
 *
 * pcscd asks "is a card there?" several times a second.  The GV-M2TV's
 * card cannot be pulled, so once the device has answered "present" the
 * answer is kept; a removable slot is asked every 500 ms.  An ask that gets
 * no answer changes nothing.  Kept apart so it runs with no device. */

#define MB8_PRESENCE_EVERY_MS      3000u
#define MB8_PRESENCE_REMOVABLE_MS  500u

struct mb8_presence {
    int removable;
    int powered;
    int known_present;
    uint64_t next_probe;
};

enum mb8_presence_step { MB8_PRESENCE_NO, MB8_PRESENCE_YES,
                         MB8_PRESENCE_ASK };

enum mb8_presence_step mb8_presence_plan(struct mb8_presence *p,
                                         uint64_t now_ms);
enum mb8_presence_step mb8_presence_learn(struct mb8_presence *p,
                                          int answered, int present);

#ifdef __cplusplus
}
#endif

#endif
