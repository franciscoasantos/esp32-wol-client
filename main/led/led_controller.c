#include "led_controller.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "ESP_WOL_LED";

typedef struct
{
    led_strip_handle_t strip;
    QueueHandle_t queue;
    int count;
    int pin;
    led_strip_type_t type;
    bool config_ready;
    led_color_t last_color;
} led_controller_state_t;

static led_controller_state_t led_state = {0};

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
    if (!led_state.strip)
    {
        ESP_LOGE(TAG, "LED strip not initialized");
        return false;
    }

    if (memcmp(color, &led_state.last_color, sizeof(led_color_t)) == 0)
    {
        return true;
    }

    for (int i = 0; i < led_state.count; i++)
    {
        if (led_state.type == LED_STRIP_TYPE_SK6812)
        {
            led_strip_set_pixel_rgbw(led_state.strip, i, color->red, color->green, color->blue, color->white);
        }
        else
        {
            led_strip_set_pixel(led_state.strip, i, color->red, color->green, color->blue);
        }
    }

    esp_err_t err = led_strip_refresh(led_state.strip);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to refresh LED strip (err=%s)", esp_err_to_name(err));
        return false;
    }

    led_state.last_color = *color;
    return true;
}

static void led_task(void *arg)
{
    led_color_t color;
    while (1)
    {
        if (xQueueReceive(led_state.queue, &color, portMAX_DELAY) == pdTRUE)
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
    if (!led_state.queue)
    {
        led_state.queue = xQueueCreate(2, sizeof(led_color_t));
        if (!led_state.queue)
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

    if (led_state.strip)
    {
        led_strip_clear(led_state.strip);
        led_strip_del(led_state.strip);
        led_state.strip = NULL;
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

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_state.strip);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init LED strip (err=%s)", esp_err_to_name(err));
        led_state.strip = NULL;
        led_state.config_ready = false;
        return false;
    }

    led_state.pin = led_pin;
    led_state.count = led_count;
    led_state.type = led_type;
    led_state.config_ready = true;

    ESP_LOGI(TAG, "LED strip initialized from config on GPIO %d with %d LEDs (type=%s)", led_state.pin, led_state.count, led_type_to_string(led_state.type));
    return true;
}

bool led_controller_enqueue(const led_color_t *color, int timeout_ms)
{
    if (!led_state.queue || !color)
    {
        return false;
    }

    return xQueueSend(led_state.queue, color, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool led_controller_is_configured(void)
{
    return led_state.config_ready;
}
