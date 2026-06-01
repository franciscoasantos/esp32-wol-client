#include "led_controller.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led_controller";

#define LED_QUEUE_LENGTH 8
#define EFFECT_FRAME_MS 20   // ~50 fps

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Mensagens enviadas para a led_task: ou uma cor sólida, ou um comando de efeito.
typedef enum {
    LED_MSG_COLOR = 0,
    LED_MSG_EFFECT
} led_msg_type_t;

typedef struct {
    led_msg_type_t type;
    led_color_t color;      // cor sólida (COLOR) ou cor base do efeito (EFFECT)
    led_effect_t effect;    // usado quando type == LED_MSG_EFFECT
} led_msg_t;

typedef struct {
    led_strip_handle_t strip;
    QueueHandle_t queue;
    int count;
    int pin;
    led_strip_type_t type;
    bool config_ready;
    led_color_t last_color;
} led_controller_state_t;

static led_controller_state_t led_state = {
    .strip = NULL,
    .queue = NULL,
    .count = 0,
    .pin = -1,
    .type = LED_STRIP_TYPE_WS2812B,
    .config_ready = false,
    .last_color = {0}
};

static led_model_t led_model_from_type(led_strip_type_t type)
{
    return (type == LED_STRIP_TYPE_SK6812) ? LED_MODEL_SK6812 : LED_MODEL_WS2812;
}

static bool led_apply_color(const led_color_t *color)
{
    if (!led_state.strip || !led_state.config_ready)
    {
        return false;
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
        ESP_LOGE(TAG, "Failed to refresh LED strip: %s", esp_err_to_name(err));
        return false;
    }

    led_state.last_color = *color;
    return true;
}

// ===================== EFEITOS (renderizados na led_task) =====================

// HSV -> RGB inteiro (h,s,v em 0..255). Estilo clássico de 6 setores.
static led_color_t hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v)
{
    led_color_t c = {0, 0, 0, 0};
    uint8_t region = h / 43;                 // 0..5
    uint8_t rem = (uint8_t)((h - region * 43) * 6);
    uint8_t p = (uint8_t)((v * (255 - s)) / 255);
    uint8_t q = (uint8_t)((v * (255 - (s * rem) / 255)) / 255);
    uint8_t t = (uint8_t)((v * (255 - (s * (255 - rem)) / 255)) / 255);

    switch (region)
    {
        case 0:  c.red = v; c.green = t; c.blue = p; break;
        case 1:  c.red = q; c.green = v; c.blue = p; break;
        case 2:  c.red = p; c.green = v; c.blue = t; break;
        case 3:  c.red = p; c.green = q; c.blue = v; break;
        case 4:  c.red = t; c.green = p; c.blue = v; break;
        default: c.red = v; c.green = p; c.blue = q; break;
    }
    return c;
}

// Escala um canal pelo nível mantendo piso 1 quando o canal é não-nulo,
// para que o breathing nunca chegue a apagar a fita.
static uint8_t scale_channel_min1(uint8_t base, uint8_t level)
{
    if (base == 0)
    {
        return 0;
    }
    uint16_t v = ((uint16_t)base * level) / 255;
    return (v < 1) ? 1 : (uint8_t)v;
}

static void effect_fill(led_color_t c)
{
    for (int i = 0; i < led_state.count; i++)
    {
        if (led_state.type == LED_STRIP_TYPE_SK6812)
        {
            led_strip_set_pixel_rgbw(led_state.strip, i, c.red, c.green, c.blue, 0);
        }
        else
        {
            led_strip_set_pixel(led_state.strip, i, c.red, c.green, c.blue);
        }
    }
}

// Renderiza um frame do efeito. NÃO mexe em last_color (a cor sólida fica
// preservada para quando o efeito for interrompido).
static void effect_render(led_effect_t effect, const led_color_t *base, uint16_t step)
{
    if (!led_state.strip || !led_state.config_ready || led_state.count <= 0)
    {
        return;
    }

    switch (effect)
    {
        case LED_EFFECT_BREATHING:
        {
            // Onda senoidal mapeada para [BREATHING_MIN .. 255]. Nunca apaga:
            // o piso garante brilho mínimo e scale_channel_min1 mantém >= 1.
            float phase = (float)(step % 1024) / 1024.0f * 2.0f * (float)M_PI;
            float wave = (sinf(phase) + 1.0f) / 2.0f; // 0..1
            const uint8_t BREATHING_MIN = 6;          // piso de brilho (~2%)
            uint8_t level = (uint8_t)(BREATHING_MIN + wave * (255 - BREATHING_MIN));
            led_color_t c = {
                scale_channel_min1(base->red, level),
                scale_channel_min1(base->green, level),
                scale_channel_min1(base->blue, level),
                0
            };
            effect_fill(c);
            break;
        }
        case LED_EFFECT_RAINBOW:
        {
            for (int i = 0; i < led_state.count; i++)
            {
                uint8_t h = (uint8_t)(step + (i * 256) / led_state.count);
                led_color_t c = hsv_to_rgb(h, 255, 255);
                if (led_state.type == LED_STRIP_TYPE_SK6812)
                {
                    led_strip_set_pixel_rgbw(led_state.strip, i, c.red, c.green, c.blue, 0);
                }
                else
                {
                    led_strip_set_pixel(led_state.strip, i, c.red, c.green, c.blue);
                }
            }
            break;
        }
        case LED_EFFECT_FADE:
        {
            led_color_t c = hsv_to_rgb((uint8_t)step, 255, 255);
            effect_fill(c);
            break;
        }
        default:
            return;
    }

    led_strip_refresh(led_state.strip);
}

static uint16_t effect_step_increment(led_effect_t effect)
{
    switch (effect)
    {
        // Breathing usa ciclo de 1024 passos; incremento 3 => ~6.8s por respiração
        case LED_EFFECT_BREATHING: return 3;
        case LED_EFFECT_RAINBOW:   return 2;
        case LED_EFFECT_FADE:      return 1;
        default:                   return 0;
    }
}

// Toda a animação vive aqui: a task fica bloqueada na fila quando ocioso e,
// quando um efeito está ativo, acorda a cada frame para renderizar.
static void led_task(void *arg)
{
    led_msg_t msg;
    led_effect_t active = LED_EFFECT_NONE;
    led_color_t base = {255, 255, 255, 0};
    led_color_t solid = {0, 0, 0, 0};
    uint16_t step = 0;

    const TickType_t frame_ticks = pdMS_TO_TICKS(EFFECT_FRAME_MS);

    while (1)
    {
        TickType_t wait = (active == LED_EFFECT_NONE) ? portMAX_DELAY : frame_ticks;

        if (xQueueReceive(led_state.queue, &msg, wait) == pdTRUE)
        {
            if (msg.type == LED_MSG_COLOR)
            {
                active = LED_EFFECT_NONE;
                solid = msg.color;
                led_apply_color(&msg.color);
            }
            else // LED_MSG_EFFECT
            {
                active = msg.effect;
                base = msg.color;
                step = 0;
                if (active == LED_EFFECT_NONE)
                {
                    led_apply_color(&solid); // restaura cor sólida
                }
            }
        }
        else
        {
            // timeout -> próximo frame do efeito
            effect_render(active, &base, step);
            step += effect_step_increment(active);
        }
    }
}

bool led_controller_start(void)
{
    if (led_state.queue == NULL)
    {
        led_state.queue = xQueueCreate(LED_QUEUE_LENGTH, sizeof(led_msg_t));
        if (led_state.queue == NULL)
        {
            ESP_LOGE(TAG, "Failed to create LED queue");
            return false;
        }
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        led_task,
        "led_task",
        4096,
        NULL,
        5,
        NULL,
        1);

    if (task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create LED task");
        return false;
    }

    ESP_LOGI(TAG, "LED controller task started");
    return true;
}

bool led_controller_configure(int led_pin, int led_count, led_strip_type_t led_type)
{
    if (led_pin < 0 || led_count <= 0)
    {
        ESP_LOGE(TAG, "Invalid LED config (pin=%d count=%d)", led_pin, led_count);
        return false;
    }

    if (led_state.strip != NULL)
    {
        led_strip_clear(led_state.strip);
        led_strip_del(led_state.strip);
        led_state.strip = NULL;
        led_state.config_ready = false;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = led_pin,
        .max_leds = led_count,
        .led_model = led_model_from_type(led_type),
        .color_component_format = (led_type == LED_STRIP_TYPE_SK6812)
            ? LED_STRIP_COLOR_COMPONENT_FMT_GRBW
            : LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        // ESP32 não tem RMT-DMA: o buffer é realimentado por ISR durante a
        // transmissão. Buffer maior => menos refills => menos glitches (LEDs
        // "piscando") quando o WiFi disputa o barramento/interrupções.
        // 256 symbols = 4 blocos de 64 words (~10 LEDs de folga por refill).
        .mem_block_symbols = 256,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_state.strip);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(err));
        led_state.strip = NULL;
        led_state.config_ready = false;
        return false;
    }

    led_state.pin = led_pin;
    led_state.count = led_count;
    led_state.type = led_type;
    led_state.config_ready = true;

    ESP_LOGI(TAG, "LED strip configured: pin=%d, count=%d, type=%d", led_pin, led_count, led_type);

    led_color_t off = {0};
    led_apply_color(&off);

    return true;
}

bool led_controller_enqueue(const led_color_t *color, int timeout_ms)
{
    if (led_state.queue == NULL || color == NULL)
    {
        return false;
    }

    led_msg_t msg = { .type = LED_MSG_COLOR, .color = *color, .effect = LED_EFFECT_NONE };
    if (xQueueSend(led_state.queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return false;
    }

    return true;
}

bool led_controller_set_effect(led_effect_t effect, const led_color_t *base_color, int timeout_ms)
{
    if (led_state.queue == NULL)
    {
        return false;
    }

    led_msg_t msg = { .type = LED_MSG_EFFECT, .effect = effect, .color = {255, 255, 255, 0} };
    if (base_color != NULL)
    {
        msg.color = *base_color;
    }

    if (xQueueSend(led_state.queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return false;
    }

    return true;
}

bool led_controller_is_configured(void)
{
    return led_state.config_ready;
}

led_color_t led_controller_get_current_color(void)
{
    return led_state.last_color;
}
