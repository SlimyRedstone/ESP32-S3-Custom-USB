/*
 * Decoding for what the device sends on the vendor IN endpoint.
 *
 * Four kinds of traffic share the endpoint:
 *   0x5A + uint32 LE   heartbeat counter, once per second while idle
 *   {"interrupt":...}  an unprompted GPIO interrupt report
 *   {...}              a JSON reply to a command
 *   plain text         any other asynchronous event
 */

#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>

#define PROTO_HEARTBEAT_TAG 0x5A

typedef enum {
    PROTO_HEARTBEAT,
    PROTO_INTERRUPT,   /*!< unprompted, so never a reply to a command */
    PROTO_REPLY,
    PROTO_EVENT,
} proto_kind_t;

proto_kind_t proto_classify(const unsigned char *data, int len);

/**
 * Decode a heartbeat counter. Returns false if @p data is not a heartbeat.
 */
bool proto_heartbeat_count(const unsigned char *data, int len,
                           unsigned long *out_count);

void proto_print(const unsigned char *data, int len);

#endif /* PROTO_H */
