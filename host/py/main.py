#!/usr/bin/env python3
"""
Host-side test for the ESP32-S3 vendor-specific USB device.

Requires:  pip install pyusb
Windows:   the device binds WinUSB automatically via its MS OS 2.0 descriptors,
           and pyusb needs a libusb-1.0.dll on PATH.
Linux:     add a udev rule for 303a:4001, or run as root.

Usage:
    python vendor_test.py            listen for events
    python vendor_test.py '<json>'   send one JSON command, print the reply

The wire protocol is JSON:
    {"set":{"led":"123456"}}          set the NeoPixel
    {"set":{"message":"hello"}}       print on the CDC serial port
    {"set":{"config":{...}}}          merge into /config.json and save
    {"get":"led"}                     -> {"led":"ABCDEF"}
    {"get":"config"}                  -> {"config":{...}}

Quote the argument for the shell, e.g. PowerShell:
    python vendor_test.py '{\"get\":\"config\"}'
"""

import sys
import time

import usb.core
import usb.util

VID = 0x303A
PID = 0x4001

TIMEOUT_MS = 1000

# Read a whole transfer, not a single packet: {"config":...} and interrupt
# reports are both larger than wMaxPacketSize. The device ends every transfer
# with a short packet, which terminates the read.
READ_SIZE = 512


def find_vendor_interface(dev):
    """Return (interface_number, ep_out, ep_in) for the vendor-specific interface."""
    cfg = dev.get_active_configuration()
    for intf in cfg:
        if intf.bInterfaceClass != 0xFF:
            continue
        ep_out = usb.util.find_descriptor(
            intf,
            custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
            == usb.util.ENDPOINT_OUT,
        )
        ep_in = usb.util.find_descriptor(
            intf,
            custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
            == usb.util.ENDPOINT_IN,
        )
        if ep_out is not None and ep_in is not None:
            return intf.bInterfaceNumber, ep_out, ep_in
    raise RuntimeError("no vendor-specific interface with bulk IN+OUT found")


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print(f"device {VID:04X}:{PID:04X} not found", file=sys.stderr)
        return 1

    try:
        print(f"found: {dev.manufacturer} / {dev.product} / sn={dev.serial_number}")
    except NotImplementedError:
        print(
            f"device {VID:04X}:{PID:04X} is enumerated but cannot be opened.\n"
            "No WinUSB driver is bound to it. Fixes, in order:\n"
            "  1. Bump bcdDevice in the firmware and reflash -- Windows caches\n"
            "     MS OS descriptor results per VID/PID/bcdDevice and will not re-query.\n"
            "  2. As admin: delete the matching key under\n"
            "     HKLM\\SYSTEM\\CurrentControlSet\\Control\\usbflags,\n"
            "     then 'pnputil /remove-device' the instance and replug.\n"
            "  3. Last resort: bind WinUSB manually with Zadig.",
            file=sys.stderr,
        )
        return 1

    itf_num, ep_out, ep_in = find_vendor_interface(dev)
    print(f"interface {itf_num}: OUT=0x{ep_out.bEndpointAddress:02X} "
          f"IN=0x{ep_in.bEndpointAddress:02X} mps={ep_in.wMaxPacketSize}")

    # Linux may have a kernel driver attached; detach it if so.
    if hasattr(dev, "is_kernel_driver_active"):
        try:
            if dev.is_kernel_driver_active(itf_num):
                dev.detach_kernel_driver(itf_num)
        except (NotImplementedError, usb.core.USBError):
            pass

    usb.util.claim_interface(dev, itf_num)

    try:
        # Drain anything already queued.
        while True:
            try:
                ep_in.read(READ_SIZE, timeout=100)
            except usb.core.USBError:
                break

        # One-shot mode: send the command given on the command line.
        if len(sys.argv) > 1:
            cmd = sys.argv[1].encode("ascii")
            print(f"-> {cmd!r}")
            ep_out.write(cmd, timeout=TIMEOUT_MS)
            reply = bytes(ep_in.read(READ_SIZE, timeout=TIMEOUT_MS))
            print(f"<- {reply!r}  ({reply.hex()})")
            return 0

        # Round trip: device replies 0xA5 + the payload we sent.
        payload = bytes(range(16))
        print(f"-> {payload.hex()}")
        ep_out.write(payload, timeout=TIMEOUT_MS)

        reply = bytes(ep_in.read(READ_SIZE, timeout=TIMEOUT_MS))
        print(f"<- {reply.hex()}")
        assert reply[0] == 0xA5, f"unexpected tag 0x{reply[0]:02X}"
        assert reply[1:] == payload, "echo payload mismatch"
        print("echo OK")
        while True:
            try:
                pkt = bytes(ep_in.read(READ_SIZE, timeout=1500))
            except usb.core.USBError:
                continue
            if not pkt:
                continue

            text = pkt.decode("ascii", "replace")
            if text.startswith('{"interrupt"'):
                kind = "interrupt"
            elif text.startswith("{"):
                kind = "reply"
            else:
                kind = "event"
            print(f"{kind}: {text}")
    finally:
        usb.util.release_interface(dev, itf_num)
        usb.util.dispose_resources(dev)

    return 0


if __name__ == "__main__":
    sys.exit(main())
