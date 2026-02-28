#include "led_controller.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "ESP_WOL_LED";

static led_strip_handle_t led_strip = NULL;
static QueueHandle_t led_queue = NULL;
static int configured_led_count = 0;
static int configured_led_pin = -1;
static bool led_config_ready = false;

static bool led_apply_color(const led_color_t *color)
{
    if (!led_strip)
    {
        ESP_LOGE(TAG, "LED strip not initialized");
        return false;
    }

    for (int i = 0; i < configured_led_count; i++)
    {
        led_strip_set_pixel(led_strip, i, color->red, color->green, color->blue);
    }

    esp_err_t err = led_strip_refresh(led_strip);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to refresh LED strip (err=%s)", esp_err_to_name(err));
        return false;
    }

    return true;
}

static void led_task(void *arg)
{
    led_color_t color;
    while (1)
    {
        if (xQueueReceive(led_queue, &color, portMAX_DELAY) == pdTRUE)
        {
            if (!led_apply_color(&color))
            {
                ESP_LOGE(TAG, "Failed to apply LED color");
            }
        }
    }
}

bool led_controller_start(void)
{
    if (!led_queue)
    {
        led_queue = xQueueCreate(2, sizeof(led_color_t));
        if (!led_queue)
        {
            ESP_LOGE(TAG, "Failed to create LED queue");
            return false;
        }

        xTaskCreatePinnedToCore(led_task, "led_task", 4096, NULL, 5, NULL, 1);
    }

    return true;
}

bool led_controller_configure(int led_pin, int led_count)
{
    if (led_pin < 0 || led_count <= 0)
    {
        ESP_LOGE(TAG, "Invalid LED config (pin=%d count=%d)", led_pin, led_count);
        return false;
    }

    if (led_strip)
    {
        led_strip_clear(led_strip);
        led_strip_del(led_strip);
        led_strip = NULL;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = led_pin,
        .max_leds = led_count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 128,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init LED strip (err=%s)", esp_err_to_name(err));
        led_strip = NULL;
        led_config_ready = false;
        return false;
    }

    configured_led_pin = led_pin;
    configured_led_count = led_count;
    led_config_ready = true;

    ESP_LOGI(TAG, "LED strip initialized from config on GPIO %d with %d LEDs", configured_led_pin, configured_led_count);
    return true;
}

bool led_controller_enqueue(const led_color_t *color, int timeout_ms)
{
    if (!led_queue || !color)
    {
        return false;
    }

    return xQueueSend(led_queue, color, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool led_controller_is_configured(void)
{
    return led_config_ready;
}
