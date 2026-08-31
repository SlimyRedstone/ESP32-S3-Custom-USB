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
