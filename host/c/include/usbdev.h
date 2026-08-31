/*
 * Thin wrapper over libusb-1.0 for the ESP32-S3 composite device.
 *
 * Owns discovery, the vendor-interface lookup, claiming, and bulk transfers, so
 * callers never touch libusb directly. Every function reports its own failures
 * to stderr; callers only need the return code.
 */

#ifndef USBDEV_H
#define USBDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libusb.h>

typedef struct usbdev {
    libusb_context       *ctx;
    libusb_device        *dev;
    libusb_device_handle *handle;

    int           interface;    /*!< vendor interface number   */
    unsigned char ep_out;       /*!< bulk OUT endpoint address */
    unsigned char ep_in;        /*!< bulk IN endpoint address  */
    int           max_packet;   /*!< wMaxPacketSize, normally 64 */
    bool          claimed;
} usbdev_t;

/**
 * Find, open and claim the vendor interface of the first matching device.
 *
 * The interface is located by class 0xFF rather than by number, so it follows
 * the composite layout instead of assuming one. Returns 0 on success; on
 * failure nothing needs releasing.
 */
int usbdev_open(usbdev_t *d, uint16_t vid, uint16_t pid);

void usbdev_close(usbdev_t *d);

void usbdev_print_identity(const usbdev_t *d);

/** Send one bulk transfer. Returns 0, or a negative libusb error code. */
int usbdev_send(usbdev_t *d, const void *data, size_t len, unsigned timeout_ms);

/**
 * Receive one bulk transfer into @p buf, storing its length in @p out_len.
 *
 * Returns 0 on success, LIBUSB_ERROR_TIMEOUT if nothing arrived in time, or
 * another negative libusb error code.
 */
int usbdev_recv(usbdev_t *d, void *buf, size_t cap, int *out_len,
                unsigned timeout_ms);

void usbdev_drain(usbdev_t *d);

#endif /* USBDEV_H */
