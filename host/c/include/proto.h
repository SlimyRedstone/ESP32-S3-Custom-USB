/*
 * Decoding for what the device sends on the vendor IN endpoint.
 *
 * Three kinds of traffic share the endpoint:
 *   0x5A + uint32 LE   heartbeat counter, once per second while idle
 *   {...}              a JSON reply to a command
 *   plain text         an asynchronous event, e.g. "Button Triggered"
 */

#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>

/** Tag byte prefixing the binary heartbeat packet. */
#define PROTO_HEARTBEAT_TAG 0x5A

typedef enum {
    PROTO_HEARTBEAT,
    PROTO_REPLY,
    PROTO_EVENT,
} proto_kind_t;

/** Classify one received packet. */
proto_kind_t proto_classify(const unsigned char *data, int len);

/**
 * Decode a heartbeat counter. Returns false if @p data is not a heartbeat.
 */
bool proto_heartbeat_count(const unsigned char *data, int len,
                           unsigned long *out_count);

/** Print one packet in a form suited to its kind. */
void proto_print(const unsigned char *data, int len);

#endif /* PROTO_H */
