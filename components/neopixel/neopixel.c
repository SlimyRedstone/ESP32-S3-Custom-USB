#include "neopixel.h"

#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "neopixel";

static led_strip_handle_t s_strip;
static uint32_t           s_count;
static uint32_t           s_color;

esp_err_t neopixel_init(const neopixel_config_t *config)
{
    if (config == NULL || config->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_strip != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num         = config->gpio,
        .max_leds               = config->count,
        .led_model              = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out       = false,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,  /* 10 MHz */
        .mem_block_symbols  = 64,
        .flags.with_dma    = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device: %s", esp_err_to_name(err));
        return err;
    }

    s_count = config->count;
    ESP_LOGI(TAG, "ready on GPIO%d, %u LED(s)", config->gpio, (unsigned)s_count);
    return led_strip_clear(s_strip);
}

esp_err_t neopixel_set_rgb(uint32_t rgb)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_color = rgb & 0xFFFFFF;

    /* led_strip takes plain r/g/b and handles the GRB wire order itself. */
    const uint8_t r = (rgb >> 16) & 0xFF;
    const uint8_t g = (rgb >> 8) & 0xFF;
    const uint8_t b = rgb & 0xFF;

    for (uint32_t i = 0; i < s_count; i++) {
        esp_err_t err = led_strip_set_pixel(s_strip, i, r, g, b);
        if (err != ESP_OK) {
            return err;
        }
    }
    return led_strip_refresh(s_strip);
}

uint32_t neopixel_get_rgb(void)
{
    return s_color;
}

esp_err_t neopixel_clear(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_color = 0;
    return led_strip_clear(s_strip);
}

esp_err_t neopixel_deinit(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = led_strip_del(s_strip);
    s_strip = NULL;
    s_count = 0;
    return err;
}
