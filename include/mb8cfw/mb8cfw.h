/* mb8cfw.h -- host library for tuners running MB8AC031-CFW.
 *
 * SPDX-License-Identifier: MIT
 *
 * The device is a TV tuner whose chip (MB8AC031 / MB86M0x "selene", or
 * MB8AC018 / MB86H5x "dione") runs our own firmware, MB8AC031-CFW.  The
 * firmware answers 64-byte FRQ1 frames carried over vendor request 0xBC
 * and pushes the transport stream out of a bulk IN endpoint.  This library
 * is the host end of that:
 *
 *   - finding and opening the device (libusb-1.0)
 *   - installing the firmware from an FRC1 container when it is not there
 *   - one frame in, one answer out (mb8_call), serialised per handle
 *   - built-in tuning (GV-M2TV, PIX-DT295), lock and raw C/N
 *   - streaming: a pump thread sends STREAM frames, a reader thread keeps a
 *     bulk read waiting, and whole 188-byte packets land in a ring buffer
 *   - the card slot: power, ATR, T=1 (mb8card.h)
 *
 * Every function that can fail returns a negative MB8_E_* and leaves a
 * message in mb8_error().  Nothing here prints.
 */
#ifndef MB8CFW_H
#define MB8CFW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB8CFW_VERSION_STRING "0.1.0"

/* Results.  The firmware's own statuses (FW_*) map onto the first seven. */
#define MB8_OK            0
#define MB8_E_NOTIMPL   (-1)    /* the firmware knows it but has no body  */
#define MB8_E_IO        (-2)    /* the hardware did not answer             */
#define MB8_E_TIMEOUT   (-3)
#define MB8_E_INVAL     (-4)
#define MB8_E_AGAIN     (-5)
#define MB8_E_BADCMD    (-6)
#define MB8_E_BUSY      (-7)
#define MB8_E_USB       (-20)   /* the USB transfer itself failed          */
#define MB8_E_SILENT    (-21)   /* nothing of ours answered the frame      */
#define MB8_E_NOCFW     (-22)   /* the hook does not jump to CFW           */
#define MB8_E_NODEV     (-23)   /* no such device, or it cannot be opened  */
#define MB8_E_NOMEM     (-24)
#define MB8_E_UNSUPP    (-25)   /* not on this model (see the model table) */
#define MB8_E_FILE      (-26)   /* a file could not be read or is wrong    */
#define MB8_E_STATE     (-27)   /* wrong order (stream not running, ...)   */

const char *mb8_strerror(int rc);

/* A monotonic clock in ms, for callers that pace themselves. */
uint64_t mb8_clock_ms(void);

/* --- models ------------------------------------------------------------ */

enum mb8_gen {
    MB8_GEN_SELENE = 0,         /* MB8AC031 / MB86M0x                     */
    MB8_GEN_DIONE  = 1          /* MB8AC018 / MB86H5x                     */
};

/* What the driver can do on a model.  Only what was run on a real device
 * with CFW is set; everything else is refused with MB8_E_UNSUPP. */
#define MB8_CAN_STREAM  0x01u   /* STREAM moves the TS                    */
#define MB8_CAN_TUNE    0x02u   /* the host has a tuning sequence         */
#define MB8_CAN_CARD    0x04u   /* the card frames work                   */
#define MB8_CAN_TUNE_BUILTIN 0x08u /* the tuning sequence is in the library
                                    * (every model with MB8_CAN_TUNE now) */
#define MB8_CAN_SAT     0x10u   /* satellite (ISDB-S): mb8_tune_sat       */

struct mb8_model {
    const char *name;           /* short name: "m2tv", "dt295", ...        */
    const char *product;        /* "GV-M2TV", ...                          */
    uint16_t vid, pid;
    enum mb8_gen gen;
    uint32_t hook;              /* where frc1_load / mb8_install put our stub */
    uint32_t mbox[4];           /* the four mailboxes                     */
    unsigned can;               /* MB8_CAN_*                              */
};

/* The table, and a lookup by short name or "vvvv:pppp". */
const struct mb8_model *mb8_models(size_t *count);
const struct mb8_model *mb8_model_find(const char *name);

/* Mailbox roles.  Two programs on one mailbox overwrite each other's
 * frames; one each and they coexist (the card driver records nothing, the
 * recorder touches no card). */
#define MB8_BOX_MAIN    0       /* control and stream                     */
#define MB8_BOX_CARD    1       /* the card driver                        */
#define MB8_BOX_SPARE   3

/* --- the handle -------------------------------------------------------- */

typedef struct mb8_dev mb8_dev;

struct mb8_open_opts {
    const char *model;          /* short name or "vvvv:pppp"; NULL = the
                                   first model in the table that opens   */
    unsigned mailbox;           /* MB8_BOX_*                              */
    int index;                  /* the n-th device of that model (0 = first) */
};

int  mb8_open(mb8_dev **out, const struct mb8_open_opts *opts);
void mb8_close(mb8_dev *d);

/* The message of the last failure on this handle (never NULL). */
const char *mb8_error(const mb8_dev *d);
/* For failures before a handle exists (mb8_open). */
const char *mb8_open_error(void);

const struct mb8_model *mb8_model_of(const mb8_dev *d);

/* --- raw access -------------------------------------------------------- */

/* Read-only: is our stub at the hook?  1 yes, 0 no, <0 the read failed. */
int mb8_hooked(mb8_dev *d);

/* One FRQ1 frame.  Returns the firmware's status mapped to MB8_OK or
 * MB8_E_*, or MB8_E_USB / MB8_E_SILENT.  `reply` receives the 40-byte data
 * field whenever the firmware answered (may be NULL).  Serialised with the
 * stream pump. */
int mb8_call(mb8_dev *d, uint32_t cmd, uint32_t arg0, uint32_t arg1,
             const uint8_t *data, size_t ndata, uint8_t reply[40]);

/* One I2C transfer through the firmware (I2CXFER). */
int mb8_i2c(mb8_dev *d, unsigned addr7, const uint8_t *w, unsigned wn,
            uint8_t *r, unsigned rn);

/* --- firmware ---------------------------------------------------------- */

struct mb8_identity {
    unsigned report_version;
    unsigned protocol;
    uint32_t signature;         /* 'FR01'                                 */
    uint32_t board;             /* vid << 16 | pid the image was built for */
    uint32_t caps;              /* bit0 dione image, bit1 card            */
    uint32_t build;             /* crc32 of the image's .text             */
};

int mb8_identify(mb8_dev *d, struct mb8_identity *id);
int mb8_start(mb8_dev *d);
int mb8_stop(mb8_dev *d);

/* Install the FRC1 container at `path` unless the same build is already
 * in.  The vendor's eight hook bytes are read off the chip and kept in
 * "<path>.hook" (only their crc32 is in the container).  `log` may be
 * NULL; it gets one line per step.  force = install even if the same build
 * answers, or the container names no loader crc.
 *
 * Before the first write it claims interface 0 and keeps it until
 * mb8_close, which is what keeps another process from installing or
 * streaming at the same time.  If another process holds it, this waits up
 * to 30 s and then gives up with MB8_E_BUSY, having written nothing. */
typedef void (*mb8_log_fn)(void *ctx, const char *line);
int mb8_install(mb8_dev *d, const char *path, int force, mb8_log_fn log,
                void *ctx);

/* Install only when the hook is not ours.  0 already there or installed. */
int mb8_ensure_firmware(mb8_dev *d, const char *path, mb8_log_fn log,
                        void *ctx);

/* --- tuning ------------------------------------------------------------ */

/* Tune to `khz` and wait up to `wait_ms` for lock.  1 locked, 0 not locked,
 * <0 failed.  The first call also initialises the front end.  Both models:
 * 470..700 MHz only (UHF 13..50 ch); outside that is not measured and is
 * refused with MB8_E_UNSUPP. */
int mb8_tune_khz(mb8_dev *d, unsigned khz, unsigned wait_ms);

/* Satellite (MB8_CAN_SAT): tune to the IF `if_khz` from the dish (BS 1 is
 * 1,049,480 kHz) and pick a TS.  `ts` 0..7 is the n-th TS the transponder
 * lists (BS carries several; CS one), anything else a TSID.  1 locked to
 * it, 0 not within `wait_ms`, <0 failed.  The dish's power is not ours: the
 * LNB must be fed from elsewhere (not measured). */
int mb8_tune_sat(mb8_dev *d, unsigned if_khz, unsigned ts, unsigned wait_ms);

/* The TSID of the TS the last mb8_tune_sat locked to (0: none). */
unsigned mb8_sat_tsid(mb8_dev *d);

/* Lock and the demodulator's raw C/N (bigger is worse; not dB).  After
 * mb8_tune_sat, the satellite demodulator's. */
struct mb8_signal {
    int locked;
    uint32_t cnr_raw;
    int cnr_valid;
};
int mb8_signal(mb8_dev *d, struct mb8_signal *s);

/* Channel names: "27" / "U27" (UHF 13..62), "C13".."C63" (CATV
 * pass-through), "557143k" (kHz).  0 when it is none of those.  Parsing
 * is all this does: tuning refuses what a model has not been measured on
 * (both models: 700 MHz and up, and CATV) with MB8_E_UNSUPP. */
unsigned mb8_channel_khz(const char *name);

/* Any channel: the names above, plus "BS1".."BS23" (odd) with an optional
 * "/TS<n>" (the n-th TS, 0..7) or "/0x<TSID>", and "CS2".."CS24" (even).
 * 0 ok, MB8_E_INVAL when it is none of those. */
struct mb8_channel {
    int sat;                    /* 0 terrestrial, 1 satellite             */
    unsigned khz;               /* RF (terrestrial) or IF (satellite)     */
    unsigned ts;                /* satellite: as mb8_tune_sat's `ts`      */
};
int mb8_channel_parse(const char *name, struct mb8_channel *c);

/* mb8_tune_khz or mb8_tune_sat, as the channel says. */
int mb8_tune_channel(mb8_dev *d, const struct mb8_channel *c,
                     unsigned wait_ms);

/* --- streaming --------------------------------------------------------- */

struct mb8_stream_opts {
    size_t buffer_bytes;        /* host ring; 0 = 32 MiB                  */
    unsigned bursts;            /* per STREAM frame; 0 = firmware default */
    unsigned ring_mib;          /* dione capture ring; 0 = 16             */
};

int mb8_stream_start(mb8_dev *d, const struct mb8_stream_opts *o);
int mb8_stream_stop(mb8_dev *d);
int mb8_stream_running(const mb8_dev *d);

/* Copy out up to `cap` bytes of whole packets.  Waits up to `timeout_ms`
 * for at least one packet (0 = do not wait).  Returns the bytes copied,
 * or <0 when the stream died (mb8_error says why). */
long mb8_stream_read(mb8_dev *d, uint8_t *buf, size_t cap,
                     unsigned timeout_ms);

/* Bytes of whole packets waiting. */
size_t mb8_stream_ready(mb8_dev *d);
void   mb8_stream_purge(mb8_dev *d);

struct mb8_stream_stats {
    uint64_t bytes_in;          /* off the endpoint                       */
    uint64_t packets;           /* whole packets delivered to the ring    */
    uint64_t dropped_bytes;     /* not on a packet boundary               */
    uint64_t overflow_bytes;    /* the host ring was full                 */
    unsigned resyncs;
    unsigned frames;            /* STREAM frames sent                     */
    unsigned fw_fulls;          /* dione: the capture ring was full       */
    unsigned fw_teis;           /* dione: packets with TEI                */
};
void mb8_stream_stats(mb8_dev *d, struct mb8_stream_stats *s);

#ifdef __cplusplus
}
#endif

#endif /* MB8CFW_H */
