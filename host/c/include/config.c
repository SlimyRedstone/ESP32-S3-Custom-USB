#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

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

const char *config_basename(const char *path)
{
    const char *last = path;

    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == 0x5C) {   /* forward slash or backslash */
            last = p + 1;
        }
    }
    return last;
}

bool config_add_app(config_slider_t *slider, const char *path)
{
    if (slider == NULL || path == NULL || path[0] == 0) {
        return false;
    }
    if (slider->app_count >= CONFIG_APPS_MAX) {
        return false;
    }

    for (int i = 0; i < slider->app_count; i++) {
        if (strcmp(slider->apps[i].path, path) == 0) {
            return false;
        }
    }

    config_app_t *app = &slider->apps[slider->app_count];
    snprintf(app->path, CONFIG_PATH_MAX, "%s", path);
    snprintf(app->name, CONFIG_NAME_MAX, "%s", config_basename(path));
    slider->app_count++;
    return true;
}

void config_remove_app(config_slider_t *slider, int index)
{
    if (slider == NULL || index < 0 || index >= slider->app_count) {
        return;
    }

    for (int i = index; i < slider->app_count - 1; i++) {
        slider->apps[i] = slider->apps[i + 1];
    }
    slider->app_count--;
}

void config_defaults(config_slider_t *out, int count, bool *debug)
{
    int known = (int)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0]));

    if (debug) {
        *debug = CONFIG_DEBUG_DEFAULT;
    }

    for (int i = 0; i < count; i++) {
        out[i].id = i;
        out[i].app_count = 0;
        if (i < known) {
            out[i].value = DEFAULTS[i].value;
            snprintf(out[i].name, CONFIG_NAME_MAX, "%s", DEFAULTS[i].name);
        } else {
            out[i].value = 0;
            snprintf(out[i].name, CONFIG_NAME_MAX, "Slider %d", i + 1);
        }
    }
}

/* Read the whole file. Returns NULL on any failure; caller frees. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }

    char *text = malloc(FILE_MAX);
    if (text == NULL) {
        fclose(f);
        return NULL;
    }

    size_t len = fread(text, 1, FILE_MAX - 1, f);
    fclose(f);
    text[len] = '\0';
    return text;
}

/* Copy a cJSON string member into a fixed buffer, leaving it alone if absent. */
static void copy_string(const cJSON *object, const char *key,
                        char *dest, size_t dest_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(dest, dest_size, "%s", item->valuestring);
    }
}

static void load_apps(const cJSON *slider, config_slider_t *out)
{
    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(slider, "apps");
    if (!cJSON_IsArray(apps)) {
        return;
    }

    out->app_count = 0;

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, apps) {
        if (!cJSON_IsObject(entry) || out->app_count >= CONFIG_APPS_MAX) {
            continue;
        }

        config_app_t *app = &out->apps[out->app_count];
        app->path[0] = 0;
        app->name[0] = 0;

        copy_string(entry, "path", app->path, CONFIG_PATH_MAX);
        copy_string(entry, "name", app->name, CONFIG_NAME_MAX);

        /* The name is only a cache of the basename, so it can be rebuilt. */
        if (app->name[0] == 0 && app->path[0] != 0) {
            snprintf(app->name, CONFIG_NAME_MAX, "%s", config_basename(app->path));
        }

        if (app->path[0] != 0) {
            out->app_count++;
        }
    }
}

bool config_load(const char *path, config_slider_t *out, int count, bool *debug)
{
    config_defaults(out, count, debug);

    char *text = read_file(path);
    if (text == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);

    if (root == NULL) {
        return false;
    }

    if (debug) {
        const cJSON *flag = cJSON_GetObjectItemCaseSensitive(root, "debug");
        if (cJSON_IsBool(flag)) {
            *debug = cJSON_IsTrue(flag) ? true : false;
        }
    }

    const cJSON *sliders = cJSON_GetObjectItemCaseSensitive(root, "sliders");
    if (!cJSON_IsArray(sliders)) {
        cJSON_Delete(root);
        return false;
    }

    int found = 0;
    int position = 0;

    const cJSON *slider = NULL;
    cJSON_ArrayForEach(slider, sliders) {
        if (!cJSON_IsObject(slider)) {
            continue;
        }

        /* Entries land by id, so the file may list them in any order; the
           position in the array is only a fallback for a missing id. */
        const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(slider, "id");
        int id = cJSON_IsNumber(id_item) ? id_item->valueint : position;
        position++;

        if (id < 0 || id >= count) {
            continue;
        }

        const cJSON *value = cJSON_GetObjectItemCaseSensitive(slider, "value");
        if (cJSON_IsNumber(value)) {
            out[id].value = value->valueint;
        }

        copy_string(slider, "name", out[id].name, CONFIG_NAME_MAX);
        load_apps(slider, &out[id]);

        out[id].id = id;
        found++;
    }

    cJSON_Delete(root);
    return found > 0;
}

bool config_save(const char *path, const config_slider_t *in, int count,
                 bool debug)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return false;
    }

    bool built = (cJSON_AddBoolToObject(root, "debug", debug) != NULL);
    cJSON *sliders = cJSON_AddArrayToObject(root, "sliders");
    built = built && (sliders != NULL);

    for (int i = 0; built && i < count; i++) {
        cJSON *slider = cJSON_CreateObject();
        if (slider == NULL || !cJSON_AddItemToArray(sliders, slider)) {
            cJSON_Delete(slider);
            built = false;
            break;
        }

        built = cJSON_AddNumberToObject(slider, "id", in[i].id) &&
                cJSON_AddNumberToObject(slider, "value", in[i].value) &&
                cJSON_AddStringToObject(slider, "name", in[i].name);

        cJSON *apps = cJSON_AddArrayToObject(slider, "apps");
        built = built && (apps != NULL);

        for (int a = 0; built && a < in[i].app_count; a++) {
            cJSON *app = cJSON_CreateObject();
            if (app == NULL || !cJSON_AddItemToArray(apps, app)) {
                cJSON_Delete(app);
                built = false;
                break;
            }

            built = cJSON_AddStringToObject(app, "path", in[i].apps[a].path) &&
                    cJSON_AddStringToObject(app, "name", in[i].apps[a].name);
        }
    }

    char *text = built ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);

    if (text == NULL) {
        fprintf(stderr, "could not build %s\n", path);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "could not write %s\n", path);
        cJSON_free(text);
        return false;
    }

    fputs(text, f);
    cJSON_free(text);

    bool ok = (ferror(f) == 0);
    fclose(f);
    return ok;
}
