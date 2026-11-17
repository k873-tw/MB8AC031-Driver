/* test_frame.c -- frames, statuses and the model table, no device.
 *
 * SPDX-License-Identifier: MIT
 */
/* The checks are the test: keep them in a Release build too. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "devpriv.h"
#include "frq1.h"

int main(void)
{
    uint8_t f[64];
    const struct mb8_model *m;
    size_t n;

    frq1_build(f, 7, FRQ1_CMD_STREAM, 64, 16, (const uint8_t *)"\x01", 1);
    assert(frq1_get32(f, 0) == 0x46525131u);
    assert(f[0x10 + 3] == 64 && f[0x14 + 3] == 16 && f[0x18] == 1);
    /* our own request read back is not an answer */
    assert(frq1_reply_check(f, 7, FRQ1_CMD_STREAM) != NULL);
    frq1_put32(f, FRQ1_FR_STATUS, 1);
    assert(frq1_reply_check(f, 7, FRQ1_CMD_STREAM) == NULL);
    assert(frq1_reply_check(f, 8, FRQ1_CMD_STREAM) != NULL);
    /* a changed command word is not our firmware answering */
    frq1_put32(f, FRQ1_FR_COMMAND, 1);
    assert(frq1_reply_check(f, 7, FRQ1_CMD_STREAM) != NULL);

    assert(mb8__status(1) == MB8_OK);
    assert(mb8__status(-5) == MB8_E_AGAIN);
    assert(mb8__status(-7) == MB8_E_BUSY);
    assert(mb8__status(-99) == MB8_E_IO);

    mb8_models(&n);
    assert(n >= 2);
    m = mb8_model_find("m2tv");
    assert(m && m->gen == MB8_GEN_DIONE && m->mbox[1] == 0x000C5040u);
    assert((m->can & MB8_CAN_TUNE) && (m->can & MB8_CAN_TUNE_BUILTIN));
    assert(!(m->can & MB8_CAN_SAT));
    m = mb8_model_find("06b8:103b");
    assert(m && strcmp(m->name, "dt295") == 0);
    assert((m->can & MB8_CAN_TUNE) && (m->can & MB8_CAN_TUNE_BUILTIN));
    assert(m->can & MB8_CAN_SAT);
    assert(mb8_model_find("nope") == NULL);
    printf("test_frame: ok\n");
    return 0;
}
