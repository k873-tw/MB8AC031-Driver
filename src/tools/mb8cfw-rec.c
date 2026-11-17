/* mb8cfw-rec -- record a channel to a file or stdout.  recpt1-like:
 *
 *     mb8cfw-rec [options] <channel> <seconds|-> <out|->
 *
 * SPDX-License-Identifier: MIT
 *
 * `-` for seconds records until SIGINT / SIGTERM, or until the reader of
 * stdout goes away (EPIPE); either way STOP goes out before it exits, which
 * is how a recorder stops a tuner command.  `-` for the channel streams
 * whatever the front end is tuned to.  Progress goes to stderr only.
 *
 * On Windows, Ctrl+C / Ctrl+Break / closing the console window take the
 * place of SIGINT / SIGTERM, and stdout is switched to binary mode.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#define STDOUT_FILENO 1
#define write_fd(fd, p, n) _write((fd), (p), (unsigned)((n) > 0x40000000u ? 0x40000000u : (n)))
#define open_out(path) _open((path), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE)
#define close_fd _close
typedef int io_ret;
#else
#include <unistd.h>
#define write_fd write
#define open_out(path) open((path), O_WRONLY | O_CREAT | O_TRUNC, 0644)
#define close_fd close
typedef ssize_t io_ret;
#endif

#include "mb8cfw/mb8cfw.h"

static volatile sig_atomic_t g_quit;

#ifdef _WIN32
static BOOL WINAPI on_console(DWORD ev)
{
    g_quit = 1;
    /* For a window close / logoff / shutdown the process is ended as soon
     * as this returns; give the main loop time to send STOP first. */
    if (ev == CTRL_CLOSE_EVENT || ev == CTRL_LOGOFF_EVENT ||
        ev == CTRL_SHUTDOWN_EVENT)
        Sleep(3000);
    return TRUE;
}

static void catch_quit(void)
{
    SetConsoleCtrlHandler(on_console, TRUE);
    _setmode(_fileno(stdout), _O_BINARY);
}
#else
static void on_quit(int sig) { (void)sig; g_quit = 1; }

static void catch_quit(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_quit;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}
#endif

static void logline(void *ctx, const char *line)
{
    (void)ctx;
    fprintf(stderr, "  %s\n", line);
}

static void usage(const char *me)
{
    fprintf(stderr,
"usage: %s [options] <channel|-> <seconds|-> <out.ts|->\n"
"  --dev NAME        m2tv | dt295 | vvvv:pppp (default: first found)\n"
"  --index N         the N-th device of that model (default 0)\n"
"  --firmware FILE   FRC1 container to install when CFW is absent\n"
"                    (or $MB8CFW_FIRMWARE)\n"
"  --lock-ms N       how long to wait for lock (default 2000)\n"
"  --buffer-mib N    host ring buffer (default 32)\n"
"  --quiet           no progress\n"
"channels: 13..50 (UHF), 557143k (kHz, 470..700 MHz),\n"
"          BS1..BS23[/TS<n>|/0x<TSID>], CS2..CS24 (PIX-DT295)\n",
        me);
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        io_ret k = write_fd(fd, p, n);

        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct mb8_open_opts oo = { NULL, MB8_BOX_MAIN, 0 };
    struct mb8_stream_opts so = { 0, 0, 0 };
    struct mb8_stream_stats st;
    const char *firmware = getenv("MB8CFW_FIRMWARE");
    const char *pos[3];
    unsigned lock_ms = 2000, seconds = 0;
    struct mb8_channel ch = { 0, 0, 0 };
    int npos = 0, quiet = 0, fd, rc, i, status = 0;
    uint64_t t0, last;
    uint8_t *buf;
    mb8_dev *d;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--dev") == 0 && i + 1 < argc)
            oo.model = argv[++i];
        else if (strcmp(a, "--index") == 0 && i + 1 < argc)
            oo.index = atoi(argv[++i]);
        else if (strcmp(a, "--firmware") == 0 && i + 1 < argc)
            firmware = argv[++i];
        else if (strcmp(a, "--lock-ms") == 0 && i + 1 < argc)
            lock_ms = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (strcmp(a, "--buffer-mib") == 0 && i + 1 < argc)
            so.buffer_bytes = (size_t)strtoul(argv[++i], NULL, 0) << 20;
        else if (strcmp(a, "--quiet") == 0)
            quiet = 1;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (a[0] == '-' && a[1] != '\0') {
            usage(argv[0]);
            return 2;
        } else if (npos < 3) {
            pos[npos++] = a;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (npos != 3) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(pos[0], "-") != 0) {
        if (mb8_channel_parse(pos[0], &ch) != MB8_OK) {
            fprintf(stderr, "%s: not a channel\n", pos[0]);
            return 2;
        }
    }
    if (strcmp(pos[1], "-") != 0)
        seconds = (unsigned)strtoul(pos[1], NULL, 10);

    catch_quit();

    if (strcmp(pos[2], "-") == 0) {
        fd = STDOUT_FILENO;
    } else {
        fd = open_out(pos[2]);
        if (fd < 0) {
            fprintf(stderr, "%s: %s\n", pos[2], strerror(errno));
            return 1;
        }
    }

    rc = mb8_open(&d, &oo);
    if (rc != MB8_OK) {
        fprintf(stderr, "open: %s\n", mb8_open_error());
        return 1;
    }
    if (!quiet)
        fprintf(stderr, "device: %s\n", mb8_model_of(d)->product);
    rc = mb8_ensure_firmware(d, firmware, quiet ? NULL : logline, NULL);
    if (rc != MB8_OK) {
        fprintf(stderr, "firmware: %s\n", mb8_error(d));
        status = 1;
        goto out;
    }
    if (ch.khz != 0) {
        rc = mb8_tune_channel(d, &ch, lock_ms);
        if (rc < 0) {
            fprintf(stderr, "tune: %s\n", mb8_error(d));
            status = 1;
            goto out;
        }
        if (!quiet && ch.sat && rc)
            fprintf(stderr, "tuned %s (IF %u kHz): locked, TSID 0x%04x\n",
                    pos[0], ch.khz, mb8_sat_tsid(d));
        else if (!quiet)
            fprintf(stderr, "tuned %s (%s%u kHz): %s\n", pos[0],
                    ch.sat ? "IF " : "", ch.khz, rc ? "locked" : "NO LOCK");
        if (rc == 0) {
            status = 1;
            goto out;
        }
    }

    rc = mb8_stream_start(d, &so);
    if (rc != MB8_OK) {
        fprintf(stderr, "stream: %s\n", mb8_error(d));
        status = 1;
        goto out;
    }
    buf = malloc(1u << 20);
    if (buf == NULL) {
        status = 1;
        goto stop;
    }
    {
        t0 = last = mb8_clock_ms();
        while (!g_quit && (seconds == 0 || mb8_clock_ms() - t0 <
                           (uint64_t)seconds * 1000u)) {
            long n = mb8_stream_read(d, buf, 1u << 20, 500);

            if (n < 0) {
                fprintf(stderr, "stream: %s\n", mb8_error(d));
                status = 1;
                break;
            }
            if (n > 0 && write_all(fd, buf, (size_t)n) != 0) {
                if (errno != EPIPE) {
                    fprintf(stderr, "write: %s\n", strerror(errno));
                    status = 1;
                }
                break;
            }
            if (!quiet && mb8_clock_ms() - last >= 5000u) {
                last = mb8_clock_ms();
                mb8_stream_stats(d, &st);
                fprintf(stderr, "  %4us  %8.1f MB  resync %u  overflow %llu\n",
                        (unsigned)((last - t0) / 1000u),
                        (double)st.bytes_in / 1e6, st.resyncs,
                        (unsigned long long)st.overflow_bytes);
            }
        }
    }
    free(buf);
stop:
    mb8_stream_stats(d, &st);
    (void)mb8_stream_stop(d);
    if (!quiet)
        fprintf(stderr, "packets %llu, resyncs %u, dropped %llu bytes, "
                "overflow %llu bytes, frames %u, fw TEI %u, fw ring full %u\n",
                (unsigned long long)st.packets, st.resyncs,
                (unsigned long long)st.dropped_bytes,
                (unsigned long long)st.overflow_bytes, st.frames, st.fw_teis,
                st.fw_fulls);
out:
    mb8_close(d);
    if (fd != STDOUT_FILENO)
        close_fd(fd);
    return status;
}
