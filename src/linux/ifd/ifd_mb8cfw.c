/* ifd_mb8cfw.c -- a pcsc-lite IFD handler: the tuner's card slot as a
 * PC/SC smart card reader.
 *
 * SPDX-License-Identifier: MIT
 *
 * pcscd loads this bundle (ifd-mb8cfw.bundle/Contents/Linux/
 * libifd_mb8cfw.so) for the USB ids its Info.plist lists, and calls the
 * IFDH* functions below.  Layers:
 *
 *     PC/SC (APDU)         this file
 *     T=1, ATR, presence   libmb8cfw (mb8card.h)
 *     CFW's card frames    libmb8cfw, mailbox 1
 *     0xBC window          libusb
 *
 * THE USB DEVICE IS OPENED PER CALL, NOT HELD.  A recorder streams through
 * the same device (mailbox 0, the bulk endpoint); this driver opens it only
 * inside a PC/SC call, uses mailbox 1 and EP0 only, and closes it again.
 * Before any frame it reads the hook: no CFW, no frame (so it cannot land
 * in the middle of an install).
 *
 * APDUS ARE REFUSED UNLESS ENABLED.  The card in these slots is a B-CAS
 * card, private use only.  Power-up, the ATR and S(IFS) always work;
 * IFDHTransmitToICC refuses APDUs unless the installed bundle's Info.plist
 * has <key>CFWAllowAPDU</key><true/>.
 *
 * Logs go to syslog as "ifd-mb8cfw".
 */
#define _GNU_SOURCE 1           /* dladdr */

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <ifdhandler.h>
#include <reader.h>

#include "mb8cfw/mb8card.h"
#include "mb8cfw/mb8cfw.h"

/* Only the IFDH* entry points leave the .so (everything else is built
 * with hidden visibility). */
#define IFD_EXPORT      __attribute__((visibility("default")))

#define MAX_SLOTS       4
#define MAX_APDU        261u
#define MAX_RESP        (65536u + 2u)
#define ATR_LIMIT_MS    3000u

struct slot {
    int used;
    DWORD lun;
    const struct mb8_model *model;
    int index;                  /* n-th device of the model              */
    mb8_dev *d;                 /* only inside a call                    */
    mb8_card *c;
    struct mb8_presence pr;
    uint8_t atr[MB8_ATR_MAX];
    size_t natr;
    mb8_card *keep;             /* the T=1 state, across calls (c = keep
                                   while a call has the device open)   */
};

static struct slot g_slots[MAX_SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_allow_apdu = -1;   /* -1 not read yet                       */
static int g_removable = -1;    /* -1 the model decides                  */

static void logf_(int prio, const char *fmt, ...)
{
    va_list ap;

    openlog("ifd-mb8cfw", LOG_PID, LOG_DAEMON);
    va_start(ap, fmt);
    vsyslog(prio, fmt, ap);
    va_end(ap);
}
#define LOG(...)  logf_(LOG_INFO, __VA_ARGS__)
#define LOGE(...) logf_(LOG_ERR, __VA_ARGS__)

/* --- the bundle's own keys ---------------------------------------------- */

static const char *plist_value(const char *text, const char *name)
{
    char key[64];
    const char *p;

    snprintf(key, sizeof(key), "<key>%s</key>", name);
    p = strstr(text, key);
    if (p == NULL)
        return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return *p == '<' ? p : NULL;
}

/* The library is <bundle>/Contents/Linux/<name>: three levels up. */
static void read_config(void)
{
    Dl_info info;
    char path[1100], text[8192];
    const char *v;
    size_t got;
    FILE *f;
    int i;

    g_allow_apdu = 0;
    if (dladdr((const void *)&g_slots, &info) == 0 || info.dli_fname == NULL)
        return;
    snprintf(path, sizeof(path), "%s", info.dli_fname);
    for (i = 0; i < 3; i++) {
        char *p = strrchr(path, '/');

        if (p == NULL)
            return;
        *p = '\0';
    }
    strncat(path, "/Contents/Info.plist", sizeof(path) - strlen(path) - 1);
    f = fopen(path, "r");
    if (f == NULL)
        return;
    got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[got] = '\0';
    v = plist_value(text, "CFWAllowAPDU");
    g_allow_apdu = v != NULL && strncmp(v, "<true/>", 7) == 0;
    v = plist_value(text, "CFWCardRemovable");
    if (v != NULL)
        g_removable = strncmp(v, "<true/>", 7) == 0;
}

/* --- slots and the device ---------------------------------------------- */

static struct slot *slot_of(DWORD lun)
{
    int i;

    for (i = 0; i < MAX_SLOTS; i++)
        if (g_slots[i].used && g_slots[i].lun == lun)
            return &g_slots[i];
    return NULL;
}

/* pcscd names the device "usb:vvvv/pppp:libudev:..." (or "...:libusb-1.0:
 * bus:addr").  The id picks the model; the n-th slot of that model opens
 * the n-th device. */
static const struct mb8_model *model_of_name(const char *name)
{
    unsigned vid, pid;
    char id[16];
    const char *p = name ? strstr(name, "usb:") : NULL;

    if (p == NULL || sscanf(p, "usb:%4x/%4x", &vid, &pid) != 2)
        return mb8_model_find("m2tv");
    snprintf(id, sizeof(id), "%04x:%04x", vid, pid);
    return mb8_model_find(id);
}

/* Open, check the hook.  0, -1 no device, -2 no CFW (closed again). */
static int dev_begin(struct slot *s)
{
    struct mb8_open_opts oo = { s->model->name, MB8_BOX_CARD, s->index };
    int rc;

    if (mb8_open(&s->d, &oo) != MB8_OK) {
        LOGE("open %s: %s", s->model->name, mb8_open_error());
        return -1;
    }
    rc = mb8_hooked(s->d);
    if (rc != 1) {
        LOG("%s", mb8_error(s->d));
        mb8_close(s->d);
        s->d = NULL;
        return -2;
    }
    if (s->keep == NULL && mb8_card_new(&s->keep, s->d) != MB8_OK) {
        LOGE("%s", mb8_error(s->d));
        mb8_close(s->d);
        s->d = NULL;
        return -1;
    }
    s->c = s->keep;
    mb8_card_rebind(s->c, s->d);
    return 0;
}

static void dev_end(struct slot *s)
{
    if (s->d != NULL)
        mb8_close(s->d);
    s->d = NULL;
}

static void forget(struct slot *s)
{
    s->pr.powered = 0;
    s->pr.known_present = 0;
    s->pr.next_probe = 0;
    s->natr = 0;
}

/* --- channel ----------------------------------------------------------- */

static RESPONSECODE create(DWORD lun, const char *name)
{
    struct slot *s = NULL;
    int i, same = 0;

    pthread_mutex_lock(&g_lock);
    if (g_allow_apdu < 0)
        read_config();
    for (i = 0; i < MAX_SLOTS && s == NULL; i++)
        if (!g_slots[i].used)
            s = &g_slots[i];
    if (s == NULL) {
        pthread_mutex_unlock(&g_lock);
        return IFD_COMMUNICATION_ERROR;
    }
    memset(s, 0, sizeof(*s));
    s->model = model_of_name(name);
    if (s->model == NULL || !(s->model->can & MB8_CAN_CARD)) {
        LOGE("'%s': no card support for this device", name ? name : "");
        pthread_mutex_unlock(&g_lock);
        return IFD_NO_SUCH_DEVICE;
    }
    for (i = 0; i < MAX_SLOTS; i++)
        if (g_slots[i].used && g_slots[i].model == s->model)
            same++;
    s->index = same;
    s->lun = lun;
    s->used = 1;
    /* The GV-M2TV's card cannot be pulled; the PIX-DT295's can. */
    s->pr.removable = g_removable >= 0 ? g_removable
                      : strcmp(s->model->name, "dt295") == 0;
    LOG("lun %lx: %s #%d (%s), APDUs %s, presence %s", (unsigned long)lun,
        s->model->product, s->index, name ? name : "",
        g_allow_apdu ? "allowed" : "refused (CFWAllowAPDU)",
        s->pr.removable ? "asked every 500 ms" : "kept once seen");
    pthread_mutex_unlock(&g_lock);
    return IFD_SUCCESS;
}

IFD_EXPORT RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPSTR DeviceName)
{
    return create(Lun, DeviceName);
}

IFD_EXPORT RESPONSECODE IFDHCreateChannel(DWORD Lun, DWORD Channel)
{
    (void)Channel;
    return create(Lun, NULL);
}

IFD_EXPORT RESPONSECODE IFDHCloseChannel(DWORD Lun)
{
    struct slot *s;

    pthread_mutex_lock(&g_lock);
    s = slot_of(Lun);
    if (s != NULL) {
        if (s->pr.powered && dev_begin(s) == 0) {
            (void)mb8_card_power_off(s->c);
            dev_end(s);
        }
        if (s->keep != NULL)
            mb8_card_free(s->keep);
        memset(s, 0, sizeof(*s));
    }
    pthread_mutex_unlock(&g_lock);
    return IFD_SUCCESS;
}

/* --- capabilities ------------------------------------------------------ */

static RESPONSECODE put(PDWORD len, PUCHAR val, const void *v, size_t n)
{
    if (*len < n)
        return IFD_ERROR_INSUFFICIENT_BUFFER;
    memcpy(val, v, n);
    *len = (DWORD)n;
    return IFD_SUCCESS;
}

IFD_EXPORT RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag, PDWORD Length,
                                 PUCHAR Value)
{
    struct slot *s;
    RESPONSECODE rc;
    UCHAR one = 1, zero = 0;
    uint32_t maxin = MAX_APDU;

    pthread_mutex_lock(&g_lock);
    s = slot_of(Lun);
    switch (Tag) {
    case TAG_IFD_ATR:
    case SCARD_ATTR_ATR_STRING:
        rc = s ? put(Length, Value, s->atr, s->natr) : IFD_ERROR_TAG;
        break;
    case TAG_IFD_SLOTNUM:
        rc = put(Length, Value, &zero, 1);
        break;
    case TAG_IFD_SLOTS_NUMBER:
    case TAG_IFD_SIMULTANEOUS_ACCESS:
        rc = put(Length, Value, &one, 1);
        break;
    case TAG_IFD_THREAD_SAFE:
    case TAG_IFD_SLOT_THREAD_SAFE:
        rc = put(Length, Value, &zero, 1);
        break;
    case SCARD_ATTR_MAXINPUT:
        rc = put(Length, Value, &maxin, sizeof(maxin));
        break;
    default:
        rc = IFD_ERROR_TAG;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

IFD_EXPORT RESPONSECODE IFDHSetCapabilities(DWORD Lun, DWORD Tag, DWORD Length,
                                 PUCHAR Value)
{
    (void)Lun; (void)Tag; (void)Length; (void)Value;
    return IFD_NOT_SUPPORTED;
}

/* No PPS: the card runs in the mode its ATR names.  T=1 only. */
IFD_EXPORT RESPONSECODE IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol, UCHAR Flags,
                                       UCHAR PTS1, UCHAR PTS2, UCHAR PTS3)
{
    struct slot *s;
    RESPONSECODE rc = IFD_SUCCESS;

    (void)Flags; (void)PTS1; (void)PTS2; (void)PTS3;
    pthread_mutex_lock(&g_lock);
    s = slot_of(Lun);
    if (Protocol != SCARD_PROTOCOL_T1)
        rc = IFD_PROTOCOL_NOT_SUPPORTED;
    else if (s == NULL || !s->pr.powered)
        rc = IFD_COMMUNICATION_ERROR;
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* --- power ------------------------------------------------------------- */

IFD_EXPORT RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action, PUCHAR Atr,
                          PDWORD AtrLength)
{
    struct slot *s;
    RESPONSECODE rc;
    DWORD cap = AtrLength ? *AtrLength : 0;
    int r;

    if (AtrLength)
        *AtrLength = 0;
    pthread_mutex_lock(&g_lock);
    s = slot_of(Lun);
    if (s == NULL) {
        pthread_mutex_unlock(&g_lock);
        return IFD_NO_SUCH_DEVICE;
    }
    if (dev_begin(s) != 0) {
        forget(s);
        pthread_mutex_unlock(&g_lock);
        return IFD_COMMUNICATION_ERROR;
    }
    switch (Action) {
    case IFD_POWER_DOWN:
        r = mb8_card_power_off(s->c);
        s->pr.powered = 0;
        s->natr = 0;
        rc = r == MB8_OK ? IFD_SUCCESS : IFD_COMMUNICATION_ERROR;
        break;
    case IFD_POWER_UP:
    case IFD_RESET:
        s->pr.powered = 0;
        r = mb8_card_power_on(s->c, s->atr, &s->natr, ATR_LIMIT_MS);
        if (r != MB8_OK) {
            LOGE("power up: %s", mb8_error(s->d));
            /* Say "no card" until the next probe: pcscd marks a card that
             * did not answer as mute and does not try it again until it
             * sees it leave and come back. */
            forget(s);
            s->pr.next_probe = mb8_clock_ms() + MB8_PRESENCE_EVERY_MS;
            rc = r == MB8_E_UNSUPP ? IFD_PROTOCOL_NOT_SUPPORTED
                                   : IFD_COMMUNICATION_ERROR;
            break;
        }
        s->pr.powered = 1;
        s->pr.known_present = 1;
        if (Atr != NULL && AtrLength != NULL) {
            size_t n = cap == 0 || s->natr <= cap ? s->natr : cap;

            memcpy(Atr, s->atr, n);
            *AtrLength = (DWORD)n;
        }
        rc = IFD_SUCCESS;
        break;
    default:
        rc = IFD_ERROR_NOT_SUPPORTED;
        break;
    }
    dev_end(s);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* --- data -------------------------------------------------------------- */

IFD_EXPORT RESPONSECODE IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci,
                               PUCHAR TxBuffer, DWORD TxLength,
                               PUCHAR RxBuffer, PDWORD RxLength,
                               PSCARD_IO_HEADER RecvPci)
{
    static uint8_t resp[MAX_RESP];
    struct slot *s;
    RESPONSECODE rc = IFD_SUCCESS;
    DWORD cap = *RxLength;
    size_t n = 0;

    *RxLength = 0;
    pthread_mutex_lock(&g_lock);
    s = slot_of(Lun);
    if (s == NULL || !s->pr.powered || TxLength < 4u) {
        rc = IFD_COMMUNICATION_ERROR;
        goto out;
    }
    if (!g_allow_apdu) {
        LOG("refused APDU %02X %02X %02X %02X (%lu bytes): CFWAllowAPDU is "
            "off", TxBuffer[0], TxBuffer[1], TxBuffer[2], TxBuffer[3],
            (unsigned long)TxLength);
        rc = IFD_COMMUNICATION_ERROR;
        goto out;
    }
    if (dev_begin(s) != 0) {
        forget(s);
        rc = IFD_COMMUNICATION_ERROR;
        goto out;
    }
    if (mb8_card_transmit(s->c, TxBuffer, TxLength, resp, sizeof(resp),
                          &n) != MB8_OK) {
        LOGE("%s", mb8_error(s->d));
        rc = IFD_COMMUNICATION_ERROR;
    } else if (n > cap) {
        rc = IFD_ERROR_INSUFFICIENT_BUFFER;
    } else {
        memcpy(RxBuffer, resp, n);
        *RxLength = (DWORD)n;
        if (RecvPci != NULL) {
            RecvPci->Protocol = SendPci.Protocol;
            RecvPci->Length = sizeof(*RecvPci);
        }
    }
    dev_end(s);
out:
    pthread_mutex_unlock(&g_lock);
    return rc;
}

IFD_EXPORT RESPONSECODE IFDHControl(DWORD Lun, DWORD dwControlCode, PUCHAR TxBuffer,
                         DWORD TxLength, PUCHAR RxBuffer, DWORD RxLength,
                         LPDWORD pdwBytesReturned)
{
    (void)Lun; (void)dwControlCode; (void)TxBuffer; (void)TxLength;
    (void)RxBuffer; (void)RxLength;
    if (pdwBytesReturned)
        *pdwBytesReturned = 0;
    return IFD_ERROR_NOT_SUPPORTED;
}

/* --- presence ---------------------------------------------------------- */

IFD_EXPORT RESPONSECODE IFDHICCPresence(DWORD Lun)
{
    enum mb8_presence_step step;
    struct slot *s;

    pthread_mutex_lock(&g_lock);
    s = slot_of(Lun);
    if (s == NULL) {
        pthread_mutex_unlock(&g_lock);
        return IFD_NO_SUCH_DEVICE;
    }
    step = mb8_presence_plan(&s->pr, mb8_clock_ms());
    if (step == MB8_PRESENCE_ASK) {
        int answered = 0, present = 0;

        if (dev_begin(s) == 0) {
            answered = mb8_card_status(s->c, NULL, NULL, &present) == MB8_OK;
            if (!answered)
                LOG("presence: %s", mb8_error(s->d));
            dev_end(s);
        }
        step = mb8_presence_learn(&s->pr, answered, present);
        if (step == MB8_PRESENCE_NO)
            s->natr = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return step == MB8_PRESENCE_YES ? IFD_ICC_PRESENT : IFD_ICC_NOT_PRESENT;
}
