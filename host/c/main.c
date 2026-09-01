/*
 * Host-side client for the ESP32-S3 composite USB device, built on libusb-1.0.
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
 * Console mode lives in include/cli.[ch], the interface in include/ui.[ch].
 */

#include <stdio.h>

#include "app.h"
#include "cli.h"
#include "ui.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    cli_install_signals();

    /* if (argc > 1) {
        return cli_run(argc, argv);
    } */

    static app_t app;
    app_init(&app);
    app_connect(&app);

    int status = ui_run(&app);
    app_shutdown(&app);
    return status;
}
