#include "proto.h"

#include <stdio.h>
#include <string.h>

#define INTERRUPT_PREFIX "{\"interrupt\""

proto_kind_t proto_classify(const unsigned char *data, int len)
{
    /* The firmware emits "interrupt" as the first key, so the prefix is enough
       to tell an unprompted report from a reply without parsing JSON. */
    if (len >= (int)sizeof(INTERRUPT_PREFIX) - 1 &&
        memcmp(data, INTERRUPT_PREFIX, sizeof(INTERRUPT_PREFIX) - 1) == 0) {
        return PROTO_INTERRUPT;
    }
    if (len >= 1 && data[0] == '{') {
        return PROTO_REPLY;
    }
    return PROTO_EVENT;
}

void proto_print(const unsigned char *data, int len)
{
    switch (proto_classify(data, len)) {
    case PROTO_INTERRUPT:
        printf("interrupt: %.*s\n", len, (const char *)data);
        break;

    case PROTO_REPLY:
        printf("reply: %.*s\n", len, (const char *)data);
        break;

    case PROTO_EVENT:
    default:
        printf("event: %.*s\n", len, (const char *)data);
        break;
    }
}

void proto_framer_reset(proto_framer_t *f)
{
    f->len = 0;
}

/*
 * Length of the JSON value starting at buf[0], or 0 if it is not complete yet.
 *
 * Braces are counted, but only outside strings: a brace inside "..." is data,
 * and a quote preceded by a backslash does not end the string.
 */
static size_t json_span(const char *buf, size_t len)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (--depth == 0) {
                return i + 1;
            }
        }
    }
    return 0;
}

/* Length of the run of non-JSON text at buf[0], up to the next document. */
static size_t text_span(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '{' || buf[i] == '\n' || buf[i] == '\r') {
            return i;
        }
    }
    return len;
}

void proto_framer_push(proto_framer_t *f, const unsigned char *data, int len,
                       void (*on_message)(void *user, const unsigned char *msg,
                                          int msg_len),
                       void *user)
{
    if (len <= 0 || on_message == NULL) {
        return;
    }

    /*
     * A message longer than the buffer can never complete, so the run is
     * abandoned rather than wedging every later message behind it.
     */
    if (f->len + (size_t)len > PROTO_FRAME_MAX) {
        f->len = 0;
        if ((size_t)len > PROTO_FRAME_MAX) {
            return;
        }
    }

    memcpy(f->buf + f->len, data, (size_t)len);
    f->len += (size_t)len;

    size_t at = 0;

    while (at < f->len) {
        char c = f->buf[at];

        /* Separators between documents carry no meaning. */
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            at++;
            continue;
        }

        size_t span;

        if (c == '{') {
            span = json_span(f->buf + at, f->len - at);
            if (span == 0) {
                break;      /* incomplete: wait for the rest */
            }
        } else {
            span = text_span(f->buf + at, f->len - at);
            if (span == 0) {
                at++;
                continue;
            }
            /* Might still be growing, so hold it unless something follows. */
            if (at + span == f->len) {
                break;
            }
        }

        char message[PROTO_FRAME_MAX];
        memcpy(message, f->buf + at, span);
        message[span] = '\0';

        on_message(user, (const unsigned char *)message, (int)span);
        at += span;
    }

    /* Keep whatever is left over for the next read. */
    if (at > 0) {
        memmove(f->buf, f->buf + at, f->len - at);
        f->len -= at;
    }
}
