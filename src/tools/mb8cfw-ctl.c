/* mb8cfw-ctl -- look at and set up a device running MB8AC031-CFW.
 *
 * SPDX-License-Identifier: MIT
 *
 *   mb8cfw-ctl list                     known models and what is plugged in
 *   mb8cfw-ctl probe                    hook, IDENTIFY            (read only)
 *   mb8cfw-ctl install <file.frc> [--force]
 *   mb8cfw-ctl tune <channel>           tune and report lock
 *                                       (27, BS1/TS0, BS15/0x40F1, CS2)
 *   mb8cfw-ctl scan [--sat]             UHF 13..62 (51..62 reported as
 *                                       not on this model) or BS/CS
 *   mb8cfw-ctl signal                   lock and raw C/N          (read only)
 *   mb8cfw-ctl card                     power, ATR, S(IFS), off  (no APDU)
 *
 * Common options: --dev NAME, --index N.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mb8cfw/mb8card.h"
#include "mb8cfw/mb8cfw.h"

static void logline(void *ctx, const char *line)
{
    (void)ctx;
    printf("  %s\n", line);
}

static int fail(mb8_dev *d, const char *what)
{
    fprintf(stderr, "%s: %s\n", what, mb8_error(d));
    return 1;
}

static int cmd_list(void)
{
    size_t n, i;
    const struct mb8_model *m = mb8_models(&n);

    printf("model   product      id         chip    stream tune card\n");
    for (i = 0; i < n; i++) {
        struct mb8_open_opts oo = { m[i].name, MB8_BOX_MAIN, 0 };
        mb8_dev *d;
        int here = mb8_open(&d, &oo) == MB8_OK;

        if (here)
            mb8_close(d);
        printf("%-7s %-12s %04x:%04x  %-7s %-6s %-4s %-4s %s\n", m[i].name,
               m[i].product, m[i].vid, m[i].pid,
               m[i].gen == MB8_GEN_DIONE ? "dione" : "selene",
               m[i].can & MB8_CAN_STREAM ? "yes" : "-",
               m[i].can & MB8_CAN_TUNE ? "yes" : "-",
               m[i].can & MB8_CAN_CARD ? "yes" : "-",
               here ? "<- connected" : "");
    }
    return 0;
}

static int cmd_probe(mb8_dev *d)
{
    struct mb8_identity id;
    int rc = mb8_hooked(d);

    printf("device   %s\n", mb8_model_of(d)->product);
    if (rc < 0)
        return fail(d, "hook");
    printf("hook     %s\n", rc ? "CFW" : "vendor loader (CFW not installed)");
    if (!rc)
        return 1;
    rc = mb8_identify(d, &id);
    if (rc != MB8_OK)
        return fail(d, "IDENTIFY");
    printf("frame    v%u, report v%u\n", id.protocol, id.report_version);
    printf("board    %04x:%04x\n", (unsigned)(id.board >> 16),
           (unsigned)(id.board & 0xFFFFu));
    printf("caps     0x%x%s%s\n", (unsigned)id.caps,
           id.caps & 1u ? " dione" : "", id.caps & 2u ? " card" : "");
    printf("build    0x%08x\n", (unsigned)id.build);
    return 0;
}

static int cmd_scan_sat(mb8_dev *d)
{
    unsigned n, locked = 0;

    for (n = 1; n <= 24; n++) {
        struct mb8_channel c;
        char name[8];
        int rc;

        snprintf(name, sizeof(name), "%s%u", n & 1u ? "BS" : "CS", n);
        if (mb8_channel_parse(name, &c) != MB8_OK)
            continue;
        rc = mb8_tune_channel(d, &c, 1500u);
        if (rc < 0)
            return fail(d, "tune");
        locked += (unsigned)rc;
        if (rc)
            printf("%-5s IF %7u kHz  locked, TSID 0x%04x\n", name, c.khz,
                   mb8_sat_tsid(d));
        else
            printf("%-5s IF %7u kHz  -\n", name, c.khz);
        fflush(stdout);
    }
    printf("%u locked\n", locked);
    return 0;
}

static int cmd_scan(mb8_dev *d)
{
    unsigned c, locked = 0;

    for (c = 13u; c <= 62u; c++) {
        char name[8];
        unsigned khz;
        int rc;

        snprintf(name, sizeof(name), "%u", c);
        khz = mb8_channel_khz(name);
        rc = mb8_tune_khz(d, khz, 1200u);
        if (rc == MB8_E_UNSUPP) {
            printf("%-4s %7u kHz  (not on this model)\n", name, khz);
            continue;
        }
        if (rc < 0)
            return fail(d, "tune");
        locked += (unsigned)rc;
        printf("%-4s %7u kHz  %s\n", name, khz, rc ? "locked" : "-");
        fflush(stdout);
    }
    printf("%u locked\n", locked);
    return 0;
}

static int cmd_card(mb8_dev *d)
{
    uint8_t atr[MB8_ATR_MAX];
    size_t n = 0, i;
    unsigned stage = 0;
    uint32_t event = 0;
    int present = 0, rc;
    mb8_card *c;

    if (mb8_card_new(&c, d) != MB8_OK)
        return fail(d, "card");
    rc = mb8_card_status(c, &stage, &event, &present);
    if (rc != MB8_OK) {
        mb8_card_free(c);
        return fail(d, "CARD_STATUS");
    }
    printf("stage %u, CARD_EVENT 0x%08x (%s)\n", stage, (unsigned)event,
           present ? "present" : "no present bit");
    rc = mb8_card_power_on(c, atr, &n, 0);
    if (rc != MB8_OK) {
        mb8_card_free(c);
        return fail(d, "power on");
    }
    printf("ATR");
    for (i = 0; i < n; i++)
        printf(" %02X", atr[i]);
    printf("\nT=1 up, S(IFS %u) answered\n", MB8_CARD_IFSD);
    (void)mb8_card_power_off(c);
    mb8_card_free(c);
    return 0;
}

static void usage(const char *me)
{
    fprintf(stderr,
"usage: %s list\n"
"       %s probe | signal | card            [--dev NAME] [--index N]\n"
"       %s install <file.frc> [--force]     [--dev NAME]\n"
"       %s tune <channel> | scan [--sat]   [--dev NAME]\n"
"         channels: 27, U27, 557143k; BS1..BS23[/TS<n>|/0x<TSID>], CS2..CS24\n",
        me, me, me, me);
}

int main(int argc, char **argv)
{
    struct mb8_open_opts oo = { NULL, MB8_BOX_MAIN, 0 };
    const char *cmd = NULL, *arg = NULL;
    int force = 0, sat = 0, rc = 0, i;
    mb8_dev *d;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--dev") == 0 && i + 1 < argc)
            oo.model = argv[++i];
        else if (strcmp(a, "--index") == 0 && i + 1 < argc)
            oo.index = atoi(argv[++i]);
        else if (strcmp(a, "--force") == 0)
            force = 1;
        else if (strcmp(a, "--sat") == 0)
            sat = 1;
        else if (a[0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (cmd == NULL)
            cmd = a;
        else if (arg == NULL)
            arg = a;
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (cmd == NULL) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(cmd, "list") == 0)
        return cmd_list();
    if (strcmp(cmd, "card") == 0)
        oo.mailbox = MB8_BOX_CARD;

    if (mb8_open(&d, &oo) != MB8_OK) {
        fprintf(stderr, "open: %s\n", mb8_open_error());
        return 1;
    }
    if (strcmp(cmd, "probe") == 0) {
        rc = cmd_probe(d);
    } else if (strcmp(cmd, "install") == 0 && arg != NULL) {
        rc = mb8_install(d, arg, force, logline, NULL) == MB8_OK
             ? 0 : fail(d, "install");
    } else if ((strcmp(cmd, "tune") == 0 && arg != NULL) ||
               strcmp(cmd, "scan") == 0) {
        if (mb8_hooked(d) != 1) {
            rc = fail(d, "hook");
        } else if (strcmp(cmd, "scan") == 0) {
            rc = sat ? cmd_scan_sat(d) : cmd_scan(d);
        } else {
            struct mb8_channel c;
            int r;

            if (mb8_channel_parse(arg, &c) != MB8_OK) {
                fprintf(stderr, "%s: not a channel\n", arg);
                rc = 2;
            } else if ((r = mb8_tune_channel(d, &c, 2000u)) < 0) {
                rc = fail(d, "tune");
            } else {
                struct mb8_signal s;

                printf("%s %s%u kHz: %s", arg, c.sat ? "IF " : "", c.khz,
                       r ? "locked" : "no lock");
                if (r && c.sat)
                    printf(", TSID 0x%04x", mb8_sat_tsid(d));
                if (mb8_signal(d, &s) == MB8_OK && s.cnr_valid)
                    printf(", C/N raw 0x%06x", (unsigned)s.cnr_raw);
                printf("\n");
                rc = r ? 0 : 1;
            }
        }
    } else if (strcmp(cmd, "signal") == 0) {
        struct mb8_signal s;

        if (mb8_hooked(d) != 1)
            rc = fail(d, "hook");
        else if (mb8_signal(d, &s) != MB8_OK)
            rc = fail(d, "signal");
        else
            printf("%s, C/N raw %s0x%06x (bigger is worse, not dB)\n",
                   s.locked ? "locked" : "no lock",
                   s.cnr_valid ? "" : "(not read) ", (unsigned)s.cnr_raw);
    } else if (strcmp(cmd, "card") == 0) {
        rc = mb8_hooked(d) == 1 ? cmd_card(d) : fail(d, "hook");
    } else {
        usage(argv[0]);
        rc = 2;
    }
    mb8_close(d);
    return rc;
}
