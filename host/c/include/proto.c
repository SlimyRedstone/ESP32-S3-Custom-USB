#include "proto.h"

#include <stdio.h>

#define HEARTBEAT_LEN 5

proto_kind_t proto_classify(const unsigned char *data, int len)
{
    if (len >= HEARTBEAT_LEN && data[0] == PROTO_HEARTBEAT_TAG) {
        return PROTO_HEARTBEAT;
    }
    if (len >= 1 && data[0] == '{') {
        return PROTO_REPLY;
    }
    return PROTO_EVENT;
}

bool proto_heartbeat_count(const unsigned char *data, int len,
                           unsigned long *out_count)
{
    if (proto_classify(data, len) != PROTO_HEARTBEAT) {
        return false;
    }

    *out_count = (unsigned long)data[1]
               | ((unsigned long)data[2] << 8)
               | ((unsigned long)data[3] << 16)
               | ((unsigned long)data[4] << 24);
    return true;
}

void proto_print(const unsigned char *data, int len)
{
    unsigned long count;

    switch (proto_classify(data, len)) {
    case PROTO_HEARTBEAT:
        proto_heartbeat_count(data, len, &count);
        printf("heartbeat %lu\n", count);
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
