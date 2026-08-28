#include "jsoncmd.h"

#include <string.h>

bool jsoncmd_escape_append(char *out, size_t out_size, const char *in)
{
    size_t n = strlen(out);

    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        char esc[7];
        const char *add;

        switch (c) {
        case '"':  add = "\\\""; break;
        case '\\': add = "\\\\"; break;
        case '\b': add = "\\b";  break;
        case '\f': add = "\\f";  break;
        case '\n': add = "\\n";  break;
        case '\r': add = "\\r";  break;
        case '\t': add = "\\t";  break;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", c);
            } else {
                esc[0] = (char)c;
                esc[1] = '\0';
            }
            add = esc;
            break;
        }

        size_t add_len = strlen(add);
        if (n + add_len + 1 > out_size) {
            return false;
        }
        memcpy(out + n, add, add_len);
        n += add_len;
        out[n] = '\0';
    }
    return true;
}

/* Start a fresh command, replacing whatever was in the buffer. */
static bool begin(char *out, size_t out_size, const char *prefix)
{
    size_t len = strlen(prefix);
    if (len + 1 > out_size) {
        return false;
    }
    memcpy(out, prefix, len + 1);
    return true;
}

void jsoncmd_usage(FILE *f)
{
    fprintf(f,
        "usage:\n"
        "  main                            listen for heartbeats and events\n"
        "  main led RRGGBB                 set the NeoPixel\n"
        "  main message <text...>          print on the CDC serial port\n"
        "  main get led|config             read a value\n"
        "  main {\"get\":\"led\"}              send raw JSON verbatim\n");
}

bool jsoncmd_build(int argc, char **argv, char *out, size_t out_size)
{
    if (argc < 2 || out_size == 0) {
        jsoncmd_usage(stderr);
        return false;
    }

    const char *verb = argv[1];

    /* Raw JSON passes through untouched. */
    if (verb[0] == '{') {
        if (argc > 2) {
            fprintf(stderr,
                    "raw JSON must be a single argument -- quote it for your shell\n");
            return false;
        }
        if (!begin(out, out_size, verb)) {
            fprintf(stderr, "raw JSON is too long, max %u bytes\n",
                    (unsigned)out_size - 1);
            return false;
        }
        return true;
    }

    if (strcmp(verb, "led") == 0 && argc == 3) {
        return begin(out, out_size, "{\"set\":{\"led\":\"")
            && jsoncmd_escape_append(out, out_size, argv[2])
            && jsoncmd_escape_append(out, out_size, "\"}}");
    }

    if (strcmp(verb, "get") == 0 && argc == 3) {
        return begin(out, out_size, "{\"get\":\"")
            && jsoncmd_escape_append(out, out_size, argv[2])
            && jsoncmd_escape_append(out, out_size, "\"}");
    }

    if (strcmp(verb, "message") == 0 && argc >= 3) {
        /* Join the remaining words so quoting the sentence is optional. */
        if (!begin(out, out_size, "{\"set\":{\"message\":\"")) {
            return false;
        }
        for (int i = 2; i < argc; i++) {
            if (i > 2 && !jsoncmd_escape_append(out, out_size, " ")) {
                return false;
            }
            if (!jsoncmd_escape_append(out, out_size, argv[i])) {
                fprintf(stderr, "message is too long, max %u bytes\n",
                        (unsigned)out_size - 1);
                return false;
            }
        }
        return jsoncmd_escape_append(out, out_size, "\"}}");
    }

    jsoncmd_usage(stderr);
    return false;
}
