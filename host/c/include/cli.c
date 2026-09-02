/*
 * Console mode. The window is opened by ui_run(); nothing here touches raylib.
 */

/*
 * sigaction and friends are POSIX, not C99. glibc hides them under a strict
 * -std=c99, so ask for them before any system header is pulled in.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsoncmd.h"
#include "proto.h"
#include "cli.h"
#include "usbdev.h"

#define USB_VID         0x303A
#define USB_PID         0x4001

#define TIMEOUT_MS      1000
/*
 * Poll interval for the listener. This is the worst-case delay before ctrl-c
 * is noticed, so it is kept short; an idle wakeup costs nothing.
 */
#define LISTEN_MS       250

/* Matches JSON_BUF_MAX in the firmware. */
#define CMD_MAX         512
#define REPLY_MAX       512

/*
 * A reply has to be told apart from an interrupt report, which can land between
 * the request and the reply. Give up after this many packets rather than
 * looping forever.
 */
#define REPLY_ATTEMPTS  4

static volatile sig_atomic_t s_stop = 0;

static void on_interrupt(int sig)
{
    (void)sig;

    /* A second ctrl-c leaves immediately, so the program can never feel stuck
       even if a transfer refuses to unwind. 130 is the usual SIGINT status. */
    if (s_stop) {
        _Exit(130);
    }
    s_stop = 1;
}

void cli_install_signals(void)
{
#ifdef _WIN32
    signal(SIGINT, on_interrupt);
    signal(SIGTERM, on_interrupt);
#else
    /*
     * sigaction, not signal(): glibc's signal() installs the handler with
     * SA_RESTART, so the kernel transparently restarts the poll() inside
     * libusb instead of letting it fail with EINTR. The loop then cannot
     * notice the flag until the transfer times out on its own.
     *
     * Clearing SA_RESTART lets a blocked transfer unwind as soon as the
     * signal lands.
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_interrupt;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif
}

/*
 * Send one command and print the reply.
 *
 * Interrupt reports arriving in the gap between the write and the read are
 * skipped; without that one gets reported as the reply and the real one is left
 * queued for the next run.
 */
static int run_one_shot(usbdev_t *dev, const char *cmd)
{
    printf("-> %s\n", cmd);

    if (usbdev_send(dev, cmd, strlen(cmd), TIMEOUT_MS) != 0) {
        return 1;
    }

    for (int attempt = 0; attempt < REPLY_ATTEMPTS && !s_stop; attempt++) {
        unsigned char buf[REPLY_MAX];
        int len = 0;

        int rc = usbdev_recv(dev, buf, sizeof(buf), &len, TIMEOUT_MS);
        if (rc != 0) {
            fprintf(stderr, "IN failed: %s\n", libusb_error_name(rc));
            return 1;
        }
        /* An interrupt report is unprompted, so it is not the reply. */
        proto_kind_t kind = proto_classify(buf, len);
        if (len < 1 || kind == PROTO_INTERRUPT) {
            if (kind == PROTO_INTERRUPT) {
                proto_print(buf, len);
            }
            continue;
        }

        printf("<- ");
        proto_print(buf, len);
        return 0;
    }

    fprintf(stderr, "no reply\n");
    return 1;
}

static int run_listener(usbdev_t *dev)
{
    printf("listening (ctrl-c to stop)...\n");

    while (!s_stop) {
        unsigned char buf[REPLY_MAX];
        int len = 0;

        int rc = usbdev_recv(dev, buf, sizeof(buf), &len, LISTEN_MS);

        if (s_stop) {
            break;      /* ctrl-c landed during the transfer */
        }
        if (rc == LIBUSB_ERROR_TIMEOUT || rc == LIBUSB_ERROR_INTERRUPTED) {
            continue;   /* nothing to report, or a signal woke the poll */
        }
        if (rc != 0) {
            fprintf(stderr, "IN failed: %s\n", libusb_error_name(rc));
            return 1;
        }
        if (len > 0) {
            proto_print(buf, len);
        }
    }

    printf("stopped\n");
    return 0;
}


int cli_run(int argc, char **argv)
{
    bool listen_only = (strcmp(argv[1], "listen") == 0);

    char cmd[CMD_MAX];
    if (!listen_only && !jsoncmd_build(argc, argv, cmd, sizeof(cmd))) {
        return 1;
    }

    usbdev_t dev;
    if (usbdev_open(&dev, USB_VID, USB_PID) != 0) {
        return 1;
    }
    usbdev_print_identity(&dev);
    usbdev_drain(&dev);

    int status = listen_only ? run_listener(&dev) : run_one_shot(&dev, cmd);

    usbdev_close(&dev);
    return status;
}
