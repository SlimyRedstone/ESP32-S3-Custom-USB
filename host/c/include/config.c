#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAX 8192

static const struct {
    int value;
    const char *name;
} DEFAULTS[] = {
    { 2024, "Main Audio" },
    { 4024, "Game Audio" },
    { 1024, "Music Audio" },
    { 3024, "Discord Audio" },
};

void config_defaults(config_slider_t *out, int count, bool *debug)
{
    int known = (int)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0]));

    if (debug) {
        *debug = CONFIG_DEBUG_DEFAULT;
    }

    for (int i = 0; i < count; i++) {
        out[i].id = i;
        if (i < known) {
            out[i].value = DEFAULTS[i].value;
            snprintf(out[i].name, CONFIG_NAME_MAX, "%s", DEFAULTS[i].name);
        } else {
            out[i].value = 0;
            snprintf(out[i].name, CONFIG_NAME_MAX, "Slider %d", i + 1);
        }
    }
}

static const char *skip_space(const char *at)
{
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
        at++;
    }
    return at;
}

/*
 * Find "key" within the object starting at @p at, without straying past its
 * closing brace. Returns the first character of the value.
 */
static const char *member_of(const char *at, const char *end, const char *key)
{
    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || n >= (int)sizeof(needle)) {
        return NULL;
    }

    size_t needle_len = strlen(needle);

    for (const char *p = at; p + needle_len <= end; p++) {
        if (memcmp(p, needle, needle_len) != 0) {
            continue;
        }
        p = skip_space(p + needle_len);
        if (*p != ':') {
            continue;
        }
        return skip_space(p + 1);
    }
    return NULL;
}

/* Read a top-level boolean, leaving @p out alone when the key is absent. */
static void read_top_bool(const char *doc, const char *key, bool *out)
{
    if (out == NULL) {
        return;
    }

    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || n >= (int)sizeof(needle)) {
        return;
    }

    const char *at = strstr(doc, needle);
    if (at == NULL) {
        return;
    }

    at = skip_space(at + strlen(needle));
    if (*at != ':') {
        return;
    }
    at = skip_space(at + 1);

    if (strncmp(at, "true", 4) == 0) {
        *out = true;
    } else if (strncmp(at, "false", 5) == 0) {
        *out = false;
    }
}

static void read_string(const char *at, char *dest, size_t dest_size)
{
    if (*at != '"') {
        return;
    }
    at++;

    size_t out = 0;
    while (*at && *at != '"' && out + 1 < dest_size) {
        if (*at == '\\' && at[1]) {
            at++;
            switch (*at) {
            case 'n': dest[out++] = '\n'; break;
            case 't': dest[out++] = '\t'; break;
            default:  dest[out++] = *at;  break;
            }
            at++;
            continue;
        }
        dest[out++] = *at++;
    }
    dest[out] = '\0';
}

/* Locate the brace matching the one at @p at, ignoring braces inside strings. */
static const char *object_end(const char *at, const char *end)
{
    int depth = 0;
    bool in_string = false;

    for (const char *p = at; p < end; p++) {
        if (in_string) {
            if (*p == '\\') {
                p++;
            } else if (*p == '"') {
                in_string = false;
            }
            continue;
        }
        if (*p == '"') {
            in_string = true;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (--depth == 0) {
                return p;
            }
        }
    }
    return NULL;
}

bool config_load(const char *path, config_slider_t *out, int count, bool *debug)
{
    config_defaults(out, count, debug);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    char *text = malloc(FILE_MAX);
    if (text == NULL) {
        fclose(f);
        return false;
    }

    size_t len = fread(text, 1, FILE_MAX - 1, f);
    fclose(f);
    text[len] = '\0';

    read_top_bool(text, "debug", debug);

    const char *end = text + len;
    const char *array = strstr(text, "\"sliders\"");
    if (array == NULL) {
        free(text);
        return false;
    }

    array = skip_space(array + strlen("\"sliders\""));
    if (*array != ':') {
        free(text);
        return false;
    }
    array = skip_space(array + 1);
    if (*array != '[') {
        free(text);
        return false;
    }

    int found = 0;

    for (const char *p = array; p < end && *p && *p != ']'; ) {
        if (*p != '{') {
            p++;
            continue;
        }

        const char *close = object_end(p, end);
        if (close == NULL) {
            break;
        }

        const char *id_at = member_of(p, close, "id");
        int id = id_at ? atoi(id_at) : found;

        /* Entries land by id, so the file may list them in any order. */
        if (id >= 0 && id < count) {
            const char *value_at = member_of(p, close, "value");
            if (value_at) {
                int v = atoi(value_at);
                out[id].value = v;
            }

            const char *name_at = member_of(p, close, "name");
            if (name_at) {
                read_string(name_at, out[id].name, CONFIG_NAME_MAX);
            }

            out[id].id = id;
            found++;
        }

        p = close + 1;
    }

    free(text);
    return found > 0;
}

static void write_escaped(FILE *f, const char *text)
{
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (c < 0x20) {
                fprintf(f, "\\u%04x", c);
            } else {
                fputc(c, f);
            }
            break;
        }
    }
}

bool config_save(const char *path, const config_slider_t *in, int count,
                 bool debug)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "could not write %s\n", path);
        return false;
    }

    fprintf(f, "{\"debug\":%s,\"sliders\":[", debug ? "true" : "false");
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            fputc(',', f);
        }
        fprintf(f, "{\"id\":%d,\"value\":%d,\"name\":\"", in[i].id, in[i].value);
        write_escaped(f, in[i].name);
        fputs("\"}", f);
    }
    fputs("]}", f);

    bool ok = (ferror(f) == 0);
    fclose(f);
    return ok;
}
