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

bool jsoncmd_append(char *out, size_t out_size, const char *raw)
{
    size_t have = strlen(out);
    size_t add = strlen(raw);

    if (have + add + 1 > out_size) {
        return false;
    }
    memcpy(out + have, raw, add + 1);
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
        "  main                            open the graphical interface\n"
        "  main listen                     stream events on the console\n"
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
            && jsoncmd_append(out, out_size, "\"}}");
    }

    if (strcmp(verb, "get") == 0 && argc == 3) {
        return begin(out, out_size, "{\"get\":\"")
            && jsoncmd_escape_append(out, out_size, argv[2])
            && jsoncmd_append(out, out_size, "\"}");
    }

    if (strcmp(verb, "message") == 0 && argc >= 3) {
        /* Join the remaining words so quoting the sentence is optional. */
        if (!begin(out, out_size, "{\"set\":{\"message\":\"")) {
            return false;
        }
        for (int i = 2; i < argc; i++) {
            if (i > 2 && !jsoncmd_append(out, out_size, " ")) {
                return false;
            }
            if (!jsoncmd_escape_append(out, out_size, argv[i])) {
                fprintf(stderr, "message is too long, max %u bytes\n",
                        (unsigned)out_size - 1);
                return false;
            }
        }
        return jsoncmd_append(out, out_size, "\"}}");
    }

    jsoncmd_usage(stderr);
    return false;
}

/* ------------------------------------------------------- reply readers --- */

#define SCRATCH_MAX 600

/* Locate the character after "key": , or NULL. */
static const char *value_of(const char *doc, const char *key)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || n >= (int)sizeof(needle)) {
        return NULL;
    }

    const char *at = strstr(doc, needle);
    if (at == NULL) {
        return NULL;
    }

    at += strlen(needle);
    while (*at == ' ' || *at == '\t') {
        at++;
    }
    if (*at != ':') {
        return NULL;
    }
    at++;
    while (*at == ' ' || *at == '\t') {
        at++;
    }
    return at;
}

/* Copy the packet into a NUL-terminated scratch buffer so string.h can work. */
static const char *as_document(const unsigned char *json, int len, char *scratch,
                               size_t scratch_size)
{
    if (json == NULL || len <= 0) {
        return NULL;
    }

    size_t n = (size_t)len;
    if (n >= scratch_size) {
        n = scratch_size - 1;
    }
    memcpy(scratch, json, n);
    scratch[n] = '\0';
    return scratch;
}

const char *jsoncmd_find_string(const unsigned char *json, int len, const char *key)
{
    static char scratch[SCRATCH_MAX];
    static char result[SCRATCH_MAX];

    const char *doc = as_document(json, len, scratch, sizeof(scratch));
    if (doc == NULL) {
        return NULL;
    }

    const char *at = value_of(doc, key);
    if (at == NULL || *at != '"') {
        return NULL;
    }
    at++;

    size_t out = 0;
    while (*at && *at != '"' && out + 1 < sizeof(result)) {
        if (*at == '\\' && at[1]) {
            at++;
            switch (*at) {
            case 'n': result[out++] = '\n'; break;
            case 'r': result[out++] = '\r'; break;
            case 't': result[out++] = '\t'; break;
            default:  result[out++] = *at;  break;
            }
            at++;
            continue;
        }
        result[out++] = *at++;
    }

    if (*at != '"') {
        return NULL;    /* unterminated, so not a value we can trust */
    }
    result[out] = '\0';
    return result;
}

const char *jsoncmd_find_object(const unsigned char *json, int len, const char *key)
{
    static char scratch[SCRATCH_MAX];
    static char result[SCRATCH_MAX];

    const char *doc = as_document(json, len, scratch, sizeof(scratch));
    if (doc == NULL) {
        return NULL;
    }

    const char *at = value_of(doc, key);
    if (at == NULL || *at != '{') {
        return NULL;
    }

    /* Copy to the matching brace, ignoring braces inside strings. */
    int depth = 0;
    bool in_string = false;
    size_t out = 0;

    while (*at && out + 1 < sizeof(result)) {
        char c = *at;

        if (in_string) {
            if (c == '\\' && at[1]) {
                result[out++] = c;
                at++;
                if (out + 1 >= sizeof(result)) break;
                result[out++] = *at++;
                continue;
            }
            if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
        }

        result[out++] = c;
        at++;

        if (!in_string && depth == 0) {
            result[out] = '\0';
            return result;
        }
    }

    return NULL;    /* ran out of buffer or input before the object closed */
}
