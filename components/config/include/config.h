/*
 * JSON configuration stored on a SPIFFS volume.
 *
 * File layout, at /config.json inside the volume:
 *
 *   {"led":{"on_boot":"FF0000","on_connected":"00FF00","on_receive":"FF00FF"}}
 *
 * Colours are RRGGBB hex strings; a leading '#' is accepted when reading and
 * omitted when writing. Missing keys fall back to the defaults above, so a
 * truncated or hand-edited file still yields a usable configuration.
 *
 * The volume lives in the "storage" partition declared by partitions.csv and is
 * mounted at /spiffs, so the file's full VFS path is /spiffs/config.json.
 * config_file_path() returns it. If the partition is empty or the file is
 * missing or unparseable, defaults are written out on first boot.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED colour slots, one per device state.
 *
 * Deliberately not prefixed CONFIG_, which is reserved for Kconfig macros.
 */
typedef enum {
    LED_STATE_BOOT = 0,      /*!< Attached, but not ready to talk yet  */
    LED_STATE_CONNECTED,     /*!< Host has configured the device       */
    LED_STATE_RECEIVE,       /*!< A packet is arriving over USB        */
    LED_STATE_DISCONNECTED,  /*!< No USB host at all: bus not powered  */
    LED_STATE_MAX,
} config_led_state_t;

/** Number of sliders tracked on the device. */
#define CONFIG_SLIDER_COUNT 4

/**
 * @brief Record a slider position received from the host.
 *
 * Signature matches usb_proto_slider_set_cb_t, so it can be wired straight to
 * usb_proto_config_t::on_slider_set.
 *
 * @param id    Slider index, 0..CONFIG_SLIDER_COUNT-1.
 * @param value New position.
 */
esp_err_t config_set_slider(int id, int value);

/**
 * @brief Last recorded position of a slider, or 0 for an unknown index.
 */
int config_get_slider(int id);

/**
 * @brief Mount the SPIFFS volume and load the configuration.
 *
 * Formats the partition if it cannot be mounted, and writes the default file
 * when none exists. Safe to call once at start-up, before anything reads a
 * colour.
 */
esp_err_t config_init(void);

/**
 * @brief Unmount the volume. Unsaved changes are lost.
 */
esp_err_t config_deinit(void);

/**
 * @brief Colour for @p state as packed 0xRRGGBB.
 *
 * Returns the compiled-in default for an out-of-range state or before
 * config_init() has run, so callers never need to check for failure.
 */
uint32_t config_led_color(config_led_state_t state);

/**
 * @brief Change a colour in memory. Call config_save() to persist it.
 */
esp_err_t config_set_led_color(config_led_state_t state, uint32_t rgb);

/**
 * @brief Write the current configuration back to the file.
 */
esp_err_t config_save(void);

/**
 * @brief Re-read the file, discarding in-memory changes.
 */
esp_err_t config_reload(void);

/**
 * @brief Full VFS path of the configuration file, e.g. "/spiffs/config.json".
 */
const char *config_file_path(void);

/**
 * @brief Serialise the current configuration to a JSON object string.
 *
 * Same shape as the file on disk. The caller owns the result and must release
 * it with free(). Returns NULL on allocation failure.
 */
char *config_to_json(void);

/**
 * @brief Merge a JSON object into the configuration and persist it.
 *
 * Accepts the same shape as the file. Keys that are absent keep their current
 * value, so a partial object such as {"led":{"on_boot":"112233"}} is a valid
 * update. Returns ESP_ERR_INVALID_ARG if @p json is not a JSON object or holds
 * no recognised key.
 */
esp_err_t config_from_json(const char *json);

#ifdef __cplusplus
}
#endif
