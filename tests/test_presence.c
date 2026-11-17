/* test_presence.c -- when the IFD handler says a card is there.
 *
 * SPDX-License-Identifier: MIT
 */
/* The checks are the test: keep them in a Release build too. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mb8cfw/mb8card.h"

int main(void)
{
    struct mb8_presence p;

    /* fixed slot: asked every 3 s until seen, then never again */
    memset(&p, 0, sizeof(p));
    assert(mb8_presence_plan(&p, 1000) == MB8_PRESENCE_ASK);
    assert(mb8_presence_learn(&p, 1, 0) == MB8_PRESENCE_NO);
    assert(mb8_presence_plan(&p, 2000) == MB8_PRESENCE_NO);
    assert(mb8_presence_plan(&p, 4000) == MB8_PRESENCE_ASK);
    assert(mb8_presence_learn(&p, 1, 1) == MB8_PRESENCE_YES);
    assert(mb8_presence_plan(&p, 4001) == MB8_PRESENCE_YES);
    assert(mb8_presence_plan(&p, 99999) == MB8_PRESENCE_YES);

    /* removable: asked every 500 ms, gone drops power */
    memset(&p, 0, sizeof(p));
    p.removable = 1;
    assert(mb8_presence_plan(&p, 0) == MB8_PRESENCE_ASK);
    assert(mb8_presence_learn(&p, 1, 1) == MB8_PRESENCE_YES);
    p.powered = 1;
    assert(mb8_presence_plan(&p, 100) == MB8_PRESENCE_YES);
    assert(mb8_presence_plan(&p, 500) == MB8_PRESENCE_ASK);
    /* no answer: the last answer stands */
    assert(mb8_presence_learn(&p, 0, 0) == MB8_PRESENCE_YES);
    assert(mb8_presence_plan(&p, 1000) == MB8_PRESENCE_ASK);
    assert(mb8_presence_learn(&p, 1, 0) == MB8_PRESENCE_NO);
    assert(!p.powered);
    printf("test_presence: ok\n");
    return 0;
}
