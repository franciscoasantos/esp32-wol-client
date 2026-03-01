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
static led_strip_type_t configured_led_type = LED_STRIP_TYPE_WS2812B;
static bool led_config_ready = false;

static led_model_t led_model_from_type(led_strip_type_t led_type)
{
    switch (led_type)
    {
    case LED_STRIP_TYPE_SK6812:
        return LED_MODEL_SK6812;
    case LED_STRIP_TYPE_WS2812B:
    default:
        return LED_MODEL_WS2812;
    }
}

static const char *led_type_to_string(led_strip_type_t led_type)
{
    switch (led_type)
    {
    case LED_STRIP_TYPE_SK6812:
        return "sk6812";
    case LED_STRIP_TYPE_WS2812B:
    default:
        return "ws2812b";
    }
}

static bool led_apply_color(const led_color_t *color)
{
    if (!led_strip)
    {
        ESP_LOGE(TAG, "LED strip not initialized");
        return false;
    }

    for (int i = 0; i < configured_led_count; i++)
    {
        if (configured_led_type == LED_STRIP_TYPE_SK6812)
        {
            led_strip_set_pixel_rgbw(led_strip, i, color->red, color->green, color->blue, color->white);
        }
        else
        {
            led_strip_set_pixel(led_strip, i, color->red, color->green, color->blue);
        }
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

bool led_controller_configure(int led_pin, int led_count, led_strip_type_t led_type)
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

    led_color_component_format_t color_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    if (led_type == LED_STRIP_TYPE_SK6812)
    {
        color_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = led_pin,
        .max_leds = led_count,
        .led_model = led_model_from_type(led_type),
        .color_component_format = color_format,
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
    configured_led_type = led_type;
    led_config_ready = true;

    ESP_LOGI(TAG, "LED strip initialized from config on GPIO %d with %d LEDs (type=%s)", configured_led_pin, configured_led_count, led_type_to_string(configured_led_type));
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
