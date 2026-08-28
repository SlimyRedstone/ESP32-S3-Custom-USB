#include "usbdev.h"

#include <stdio.h>
#include <string.h>

#define DRAIN_MS 100

static void print_string_desc(libusb_device_handle *h, uint8_t index,
                              const char *label)
{
    unsigned char buf[256];

    if (index == 0) {
        printf("%s: (none)\n", label);
        return;
    }

    int rc = libusb_get_string_descriptor_ascii(h, index, buf, sizeof(buf));
    if (rc < 0) {
        printf("%s: <%s>\n", label, libusb_error_name(rc));
    } else {
        printf("%s: %.*s\n", label, rc, buf);
    }
}

static void report_open_failure(int rc, uint16_t vid, uint16_t pid)
{
    if (rc != LIBUSB_ERROR_NOT_SUPPORTED) {
        fprintf(stderr, "libusb_open: %s\n", libusb_error_name(rc));
        return;
    }

    fprintf(stderr,
        "device %04X:%04X is enumerated but cannot be opened.\n"
        "No WinUSB driver is bound to it. Fixes, in order:\n"
        "  1. Bump USB_BCD_DEVICE in the firmware and reflash -- Windows caches\n"
        "     MS OS descriptor results per VID/PID/bcdDevice and will not re-query.\n"
        "  2. As admin: delete the matching key under\n"
        "     HKLM\\SYSTEM\\CurrentControlSet\\Control\\usbflags,\n"
        "     then 'pnputil /remove-device' the instance and replug.\n"
        "  3. Last resort: bind WinUSB manually with Zadig.\n",
        vid, pid);
}

/*
 * Locate the vendor-specific interface and its bulk endpoint pair.
 *
 * The device is composite, so this walks past the CDC interfaces rather than
 * assuming the vendor function sits at a fixed index.
 */
static int find_vendor_interface(usbdev_t *d)
{
    struct libusb_config_descriptor *cfg = NULL;

    int rc = libusb_get_active_config_descriptor(d->dev, &cfg);
    if (rc != 0) {
        fprintf(stderr, "get_active_config_descriptor: %s\n",
                libusb_error_name(rc));
        return -1;
    }

    bool found = false;

    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        for (int a = 0; a < cfg->interface[i].num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor *id =
                &cfg->interface[i].altsetting[a];

            if (id->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC) {
                continue;
            }

            unsigned char out = 0, in = 0;
            int packet = 0;

            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];

                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
                        != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    in = ep->bEndpointAddress;
                } else {
                    out = ep->bEndpointAddress;
                }
                /* Both endpoints share a packet size on this device. */
                packet = ep->wMaxPacketSize;
            }

            if (out && in) {
                d->interface  = id->bInterfaceNumber;
                d->ep_out     = out;
                d->ep_in      = in;
                d->max_packet = packet;
                found = true;
            }
        }
    }

    libusb_free_config_descriptor(cfg);

    if (!found) {
        fprintf(stderr, "no vendor-specific interface with bulk IN+OUT found\n");
        return -1;
    }
    return 0;
}

/*
 * Try every device matching vid:pid rather than only the first. A second board,
 * or a stale handle held elsewhere, should not mask a usable one.
 */
static int open_matching(usbdev_t *d, uint16_t vid, uint16_t pid)
{
    libusb_device **list = NULL;

    ssize_t count = libusb_get_device_list(d->ctx, &list);
    if (count < 0) {
        fprintf(stderr, "get_device_list: %s\n", libusb_error_name((int)count));
        return -1;
    }

    int matches = 0;
    int last_rc = 0;

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(list[i], &desc) != 0) {
            continue;
        }
        if (desc.idVendor != vid || desc.idProduct != pid) {
            continue;
        }
        matches++;

        int rc = libusb_open(list[i], &d->handle);
        if (rc == 0) {
            d->dev = libusb_ref_device(list[i]);
            libusb_free_device_list(list, 1);
            return 0;
        }

        last_rc = rc;
        d->handle = NULL;
    }

    libusb_free_device_list(list, 1);

    if (matches == 0) {
        fprintf(stderr, "device %04X:%04X not found\n", vid, pid);
    } else {
        report_open_failure(last_rc, vid, pid);
    }
    return -1;
}

int usbdev_open(usbdev_t *d, uint16_t vid, uint16_t pid)
{
    memset(d, 0, sizeof(*d));

    int rc = libusb_init(&d->ctx);
    if (rc != 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
        d->ctx = NULL;
        return -1;
    }

    if (open_matching(d, vid, pid) != 0) {
        usbdev_close(d);
        return -1;
    }

    if (find_vendor_interface(d) != 0) {
        usbdev_close(d);
        return -1;
    }

    /* No-op where the platform has no kernel drivers to detach. */
    libusb_set_auto_detach_kernel_driver(d->handle, 1);

    rc = libusb_claim_interface(d->handle, d->interface);
    if (rc != 0) {
        fprintf(stderr, "claim_interface(%d): %s\n",
                d->interface, libusb_error_name(rc));
        if (rc == LIBUSB_ERROR_BUSY) {
            fprintf(stderr,
                    "another process holds the interface -- close the Python "
                    "client or the web page first\n");
        }
        usbdev_close(d);
        return -1;
    }
    d->claimed = true;

    printf("interface %d: OUT=0x%02X IN=0x%02X mps=%d\n",
           d->interface, d->ep_out, d->ep_in, d->max_packet);
    return 0;
}

void usbdev_close(usbdev_t *d)
{
    if (d->claimed) {
        libusb_release_interface(d->handle, d->interface);
        d->claimed = false;
    }
    if (d->handle) {
        libusb_close(d->handle);
        d->handle = NULL;
    }
    if (d->dev) {
        libusb_unref_device(d->dev);
        d->dev = NULL;
    }
    if (d->ctx) {
        libusb_exit(d->ctx);
        d->ctx = NULL;
    }
}

void usbdev_print_identity(const usbdev_t *d)
{
    struct libusb_device_descriptor desc;

    if (libusb_get_device_descriptor(d->dev, &desc) != 0) {
        return;
    }

    /* Read the indices from the descriptor instead of assuming 1/2/3. */
    print_string_desc(d->handle, desc.iManufacturer, "manufacturer");
    print_string_desc(d->handle, desc.iProduct,      "product");
    print_string_desc(d->handle, desc.iSerialNumber, "serial");
}

int usbdev_send(usbdev_t *d, const void *data, size_t len, unsigned timeout_ms)
{
    int transferred = 0;

    /* libusb does not modify the buffer for an OUT transfer. */
    int rc = libusb_bulk_transfer(d->handle, d->ep_out,
                                  (unsigned char *)(uintptr_t)data,
                                  (int)len, &transferred, timeout_ms);
    if (rc != 0) {
        fprintf(stderr, "OUT failed: %s\n", libusb_error_name(rc));
        return rc;
    }
    if ((size_t)transferred != len) {
        fprintf(stderr, "OUT short write: %d of %u bytes\n",
                transferred, (unsigned)len);
        return LIBUSB_ERROR_IO;
    }
    return 0;
}

int usbdev_recv(usbdev_t *d, void *buf, size_t cap, int *out_len,
                unsigned timeout_ms)
{
    int transferred = 0;

    int rc = libusb_bulk_transfer(d->handle, d->ep_in, (unsigned char *)buf,
                                  (int)cap, &transferred, timeout_ms);
    *out_len = (rc == 0) ? transferred : 0;
    return rc;
}

void usbdev_drain(usbdev_t *d)
{
    unsigned char buf[512];
    int len = 0;

    while (usbdev_recv(d, buf, sizeof(buf), &len, DRAIN_MS) == 0 && len > 0) {
        /* discard */
    }
}
