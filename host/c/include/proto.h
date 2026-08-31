/*
 * Decoding for what the device sends on the vendor IN endpoint.
 *
 * Three kinds of traffic share the endpoint:
 *   {"interrupt":...}  an unprompted GPIO interrupt report
 *   {...}              a JSON reply to a command
 *   plain text         any other asynchronous event
 */

#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>

typedef enum {
    PROTO_INTERRUPT,   /*!< unprompted, so never a reply to a command */
    PROTO_REPLY,
    PROTO_EVENT,
} proto_kind_t;

proto_kind_t proto_classify(const unsigned char *data, int len);

void proto_print(const unsigned char *data, int len);

#endif /* PROTO_H */
