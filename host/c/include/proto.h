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
#include <stddef.h>

typedef enum {
    PROTO_INTERRUPT,   /*!< unprompted, so never a reply to a command */
    PROTO_REPLY,
    PROTO_EVENT,
} proto_kind_t;

proto_kind_t proto_classify(const unsigned char *data, int len);

void proto_print(const unsigned char *data, int len);

/*
 * Reassembly for the IN endpoint.
 *
 * A bulk endpoint is a byte stream, not a datagram service, so one read is not
 * one message. Linux hands back a single short-packet-terminated transfer at a
 * time, which makes the two look equivalent; WinUSB concatenates whatever is
 * queued into the read buffer, so several documents arrive glued together and
 * a long one can be split across reads. Both are legal, and framing here is
 * what makes the host agree with the device either way.
 */

#define PROTO_FRAME_MAX 2048

typedef struct {
    char   buf[PROTO_FRAME_MAX];
    size_t len;
} proto_framer_t;

void proto_framer_reset(proto_framer_t *f);

/**
 * Feed received bytes, invoking @p on_message once per complete message.
 *
 * A trailing partial message is retained until the rest of it arrives. Text
 * that is not JSON is passed on as one message per run of it.
 *
 * @param on_message Called with a NUL-terminated message. May not be NULL.
 * @param user       Passed through untouched.
 */
void proto_framer_push(proto_framer_t *f, const unsigned char *data, int len,
                       void (*on_message)(void *user, const unsigned char *msg,
                                          int msg_len),
                       void *user);

#endif /* PROTO_H */
