#include "button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "button";

#define TASK_STACK      3072
#define TASK_PRIO       5
#define POLL_MS         20

static button_config_t s_cfg;
static button_cb_t     s_cb;
static void           *s_arg;
static TaskHandle_t    s_task;
static volatile bool   s_running;

static bool level_is_active(void)
{
    int level = gpio_get_level(s_cfg.gpio);
    return s_cfg.active_low ? (level == 0) : (level != 0);
}

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;

    BaseType_t higher_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &higher_woken);
    portYIELD_FROM_ISR(higher_woken);
}

/*
 * One event per press. After firing, wait for release before re-arming so a
 * bouncing contact cannot produce a burst of events.
 */
static void button_task(void *arg)
{
    (void)arg;

    while (s_running) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0) {
            continue;   /* timeout, just re-check s_running */
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.debounce_ms));
        if (!level_is_active()) {
            continue;   /* bounce or glitch, not a real press */
        }

        if (s_cb) {
            s_cb(s_arg);
        }

        while (s_running && level_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(s_cfg.debounce_ms));

        /* Drop notifications queued by bounces while we were waiting. */
        ulTaskNotifyTake(pdTRUE, 0);
    }

    vTaskDelete(NULL);
}

esp_err_t button_init(const button_config_t *config, button_cb_t cb, void *arg)
{
    if (config == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *config;
    s_cb  = cb;
    s_arg = arg;

    const gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << s_cfg.gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = s_cfg.active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = s_cfg.active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = s_cfg.active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_running = true;
    if (xTaskCreate(button_task, "button", TASK_STACK, NULL,
                    TASK_PRIO, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    /* Another component may already own the shared ISR service. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }

    err = gpio_isr_handler_add(s_cfg.gpio, button_isr, NULL);
    if (err != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(TAG, "GPIO%d armed, %s edge, %ums debounce",
             s_cfg.gpio, s_cfg.active_low ? "falling" : "rising",
             (unsigned)s_cfg.debounce_ms);
    return ESP_OK;

fail:
    s_running = false;
    return err;
}

esp_err_t button_deinit(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_isr_handler_remove(s_cfg.gpio);
    s_running = false;

    /* Let button_task observe the flag and delete itself. */
    vTaskDelay(pdMS_TO_TICKS(600));
    s_task = NULL;
    s_cb   = NULL;

    return gpio_reset_pin(s_cfg.gpio);
}
