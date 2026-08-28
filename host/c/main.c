/*
 * Host-side client for the ESP32-S3 composite USB device.
 * C equivalent of ../vendor_test.py, built on libusb-1.0.
 *
 * Usage:
 *   main                            open the Clay interface
 *   main listen                     stream events on the console instead
 *   main led ABCDEF                 set the NeoPixel
 *   main message hello there        print on the CDC serial port
 *   main get led                    read the current colour
 *   main get config                 read /config.json
 *   main {"get":"led"}              send raw JSON verbatim
 *
 * The wire protocol is JSON:
 *   {"set":{"led":"123456"}}       -> {"ok":true}
 *   {"set":{"message":"hello"}}    -> {"ok":true}
 *   {"set":{"config":{...}}}       -> {"ok":true}
 *   {"get":"led"}                  -> {"led":"ABCDEF"}
 *   {"get":"config"}               -> {"config":{...}}
 *
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

#include "app.h"
#include "jsoncmd.h"
#include "proto.h"
#include "ui.h"
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
 * A command reply has to be told apart from the 1 Hz heartbeat, which can land
 * between the request and the reply. Give up after this many packets rather
 * than looping forever if the device only ever sends heartbeats.
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

static void install_signal_handlers(void)
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
 * Heartbeats and interrupt reports arriving in the gap between the write and
 * the read are skipped; without that one of them gets reported as the reply and
 * the real one is left queued for the next run.
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
        /* Heartbeats and interrupt reports are unprompted, so neither is the
           reply we are waiting for. */
        proto_kind_t kind = proto_classify(buf, len);
        if (len < 1 || kind == PROTO_HEARTBEAT || kind == PROTO_INTERRUPT) {
            if (kind == PROTO_INTERRUPT) {
                proto_print(buf, len);   /* still worth showing */
            }
            continue;
        }

        printf("<- ");
        proto_print(buf, len);
        return 0;
    }

    fprintf(stderr, "no reply, only heartbeats\n");
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

int main(int argc, char **argv)
{
    /* Line buffering keeps output readable when piped into a file or tee. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Leave the interface claimed for no longer than one poll after ctrl-c. */
    install_signal_handlers();

    /* No arguments: the graphical client, which manages its own connection. */
    if (argc == 1) {
        static app_t app;
        app_init(&app);
        app_connect(&app);          /* best effort; the window opens regardless */
        int rc = ui_run(&app);
        app_shutdown(&app);
        return rc;
    }

    bool listen_only = (strcmp(argv[1], "listen") == 0);

    char cmd[CMD_MAX];
    if (!listen_only && !jsoncmd_build(argc, argv, cmd, sizeof(cmd))) {
        return 1;   /* nothing opened yet, so nothing to release */
    }

    usbdev_t dev;
    if (usbdev_open(&dev, USB_VID, USB_PID) != 0) {
        return 1;
    }
    usbdev_print_identity(&dev);

    /* Clear stale heartbeats so a reply is not queued behind them. */
    usbdev_drain(&dev);

    int status = listen_only ? run_listener(&dev) : run_one_shot(&dev, cmd);

    usbdev_close(&dev);
    return status;
}
