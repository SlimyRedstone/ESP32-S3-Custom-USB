/*
 * Persistence for the fader strip, stored beside the executable as config.json:
 *
 *   {"debug":true,"sliders":[
 *     {"id":0,"value":2024,"name":"Main Audio",
 *      "apps":[{"path":"C:/.../Discord.exe","name":"Discord.exe"}]},
 *     ...
 *   ]}
 *
 * "debug" controls whether the interface shows its traffic console.
 *
 * The shape is fixed and small, so this scans rather than pulling in a JSON
 * library: the host client would otherwise need cJSON built for every vcpkg
 * triplet and every distro it runs on.
 *
 * A missing, unreadable or malformed file is not an error. Whatever cannot be
 * read falls back to the built-in defaults, so the interface always starts.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define CONFIG_NAME_MAX 32
#define CONFIG_PATH_MAX 260
#define CONFIG_APPS_MAX 16

/* One executable whose volume a slider controls. */
typedef struct {
    char path[CONFIG_PATH_MAX];   /*!< as chosen in the file dialog */
    char name[CONFIG_NAME_MAX];   /*!< basename, what the mixer matches on */
} config_app_t;

typedef struct {
    int  id;
    int  value;
    char name[CONFIG_NAME_MAX];

    config_app_t apps[CONFIG_APPS_MAX];
    int          app_count;
} config_slider_t;

/**
 * Append an app to a slider, ignoring duplicates by path.
 *
 * @return false if the slider is full or the path is already present.
 */
bool config_add_app(config_slider_t *slider, const char *path);

/** Remove the app at @p index, closing the gap. */
void config_remove_app(config_slider_t *slider, int index);

/** Basename of @p path, without directories. */
const char *config_basename(const char *path);

#define CONFIG_DEBUG_DEFAULT true

/**
 * Fill @p out with the built-in defaults.
 *
 * @param debug Receives the default debug flag. May be NULL.
 */
void config_defaults(config_slider_t *out, int count, bool *debug);

/**
 * Read @p path into @p out.
 *
 * Entries are placed by their "id" field. Anything absent from the file keeps
 * its default, so a partially written file still yields a usable strip.
 *
 * @param debug Receives the "debug" flag, or its default when absent. May be NULL.
 * @return true if the file was read and at least one slider was recognised.
 */
bool config_load(const char *path, config_slider_t *out, int count, bool *debug);

/** Write @p in to @p path, creating it if necessary. */
bool config_save(const char *path, const config_slider_t *in, int count,
                 bool debug);

#endif /* CONFIG_H */
