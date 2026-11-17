/* test_channel.c -- channel names to frequencies.
 *
 * SPDX-License-Identifier: MIT
 */
/* The checks are the test: keep them in a Release build too. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mb8cfw/mb8cfw.h"

int main(void)
{
    /* channels */
    assert(mb8_channel_khz("13") == 473143u);
    assert(mb8_channel_khz("U27") == 557143u);
    assert(mb8_channel_khz("62") == 767143u);
    assert(mb8_channel_khz("63") == 0);
    assert(mb8_channel_khz("C13") == 111143u);
    assert(mb8_channel_khz("C22") == 167143u);
    assert(mb8_channel_khz("C23") == 225143u);
    assert(mb8_channel_khz("C63") == 465143u);
    assert(mb8_channel_khz("C64") == 0);
    assert(mb8_channel_khz("557143k") == 557143u);
    assert(mb8_channel_khz("x") == 0);
    assert(mb8_channel_khz("") == 0);
    assert(mb8_channel_khz("CS2") == 0);

    {
        struct mb8_channel c;
        assert(mb8_channel_parse("27", &c) == MB8_OK && !c.sat &&
               c.khz == 557143u);
        assert(mb8_channel_parse("C30", &c) == MB8_OK && !c.sat);
        assert(mb8_channel_parse("BS1", &c) == MB8_OK && c.sat &&
               c.khz == 1049480u && c.ts == 0);
        assert(mb8_channel_parse("BS15/TS1", &c) == MB8_OK &&
               c.khz == 1318000u && c.ts == 1);
        assert(mb8_channel_parse("bs23/0x4770", &c) == MB8_OK &&
               c.khz == 1471440u && c.ts == 0x4770u);
        assert(mb8_channel_parse("CS2", &c) == MB8_OK && c.sat &&
               c.khz == 1613000u);
        assert(mb8_channel_parse("CS24", &c) == MB8_OK && c.khz == 2053000u);
        assert(mb8_channel_parse("BS2", &c) == MB8_E_INVAL);
        assert(mb8_channel_parse("CS3", &c) == MB8_E_INVAL);
        assert(mb8_channel_parse("BS25", &c) == MB8_E_INVAL);
        assert(mb8_channel_parse("BS1/TS8", &c) == MB8_E_INVAL);
        assert(mb8_channel_parse("BS1/7", &c) == MB8_E_INVAL);
        assert(mb8_channel_parse("BS1x", &c) == MB8_E_INVAL);
        assert(mb8_channel_parse("x", &c) == MB8_E_INVAL);
    }
    printf("test_channel: ok\n");
    return 0;
}
