#include "led_controller.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

static const char *TAG = "led_controller";

#define LED_QUEUE_LENGTH 8
#define LED_MIN_FRAME_MS 20   // teto de ~50 fps para fitas curtas

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Períodos de animação em tempo de parede. Antes a velocidade vinha de um
// incremento por frame, o que fazia o mesmo efeito rodar mais devagar em fitas
// longas (o refresh de 589 LEDs RGBW leva ~23,6 ms e entrava no orçamento).
#define BREATHING_PERIOD_MS 6800
#define RAINBOW_PERIOD_MS   3000
#define FADE_PERIOD_MS      6000

// Mensagens enviadas para a led_task.
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
    // Protege o handle da fita: led_controller_configure() roda na task do
    // WebSocket e pode destruir o strip enquanto a led_task está dentro de um
    // led_strip_refresh() com o mesmo handle.
    SemaphoreHandle_t strip_mutex;
    int count;
    int pin;
    led_strip_type_t type;
    bool config_ready;
    led_color_t last_color;  // cor sólida BRUTA (sem gamma aplicado)
    uint32_t frame_ms;       // orçamento de frame, derivado do tamanho da fita
} led_controller_state_t;

static led_controller_state_t led_state = {
    .strip = NULL,
    .queue = NULL,
    .strip_mutex = NULL,
    .count = 0,
    .pin = -1,
    .type = LED_STRIP_TYPE_WS2812B,
    .config_ready = false,
    .last_color = {0},
    .frame_ms = LED_MIN_FRAME_MS
};

// Correção de gamma 2.2: o olho é logarítmico, o PWM não. Sem isto quase toda
// a variação visível de uma rampa linear se concentra no topo da escala.
static const uint8_t GAMMA8[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
      6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  11,  11,  11,  12,
     12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,
     20,  20,  21,  22,  22,  23,  23,  24,  25,  25,  26,  26,  27,  28,  28,  29,
     30,  30,  31,  32,  33,  33,  34,  35,  35,  36,  37,  38,  39,  39,  40,  41,
     42,  43,  43,  44,  45,  46,  47,  48,  49,  49,  50,  51,  52,  53,  54,  55,
     56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
     73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  85,  87,  88,  89,  90,
     91,  93,  94,  95,  97,  98,  99, 100, 102, 103, 105, 106, 107, 109, 110, 111,
    113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
    137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154, 156, 158, 159, 161,
    163, 165, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 190,
    192, 194, 196, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
    223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246, 248, 251, 253, 255,
};

static led_model_t led_model_from_type(led_strip_type_t type)
{
    return (type == LED_STRIP_TYPE_SK6812) ? LED_MODEL_SK6812 : LED_MODEL_WS2812;
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline void strip_lock(void)
{
    if (led_state.strip_mutex) xSemaphoreTake(led_state.strip_mutex, portMAX_DELAY);
}

static inline void strip_unlock(void)
{
    if (led_state.strip_mutex) xSemaphoreGive(led_state.strip_mutex);
}

// ===================== FUNIL ÚNICO DE SAÍDA =====================
// Todo pixel escrito na fita passa por aqui: envelope do efeito -> gamma.
// `level` é o envelope do efeito (255 = sem atenuação).

static inline uint8_t chan_out(uint8_t base, uint8_t level)
{
    if (base == 0)
    {
        return 0;
    }

    uint32_t x = ((uint32_t)base * level) / 255;

    uint8_t g = GAMMA8[x];
    // Piso de 1: o gamma zera entradas <= 14, e um canal aceso não deve apagar
    // por arredondamento (é o que mantinha o breathing sempre visível).
    return (g == 0 && x > 0) ? 1 : g;
}

static inline void put_pixel(int i, led_color_t c, uint8_t level)
{
    if (led_state.type == LED_STRIP_TYPE_SK6812)
    {
        led_strip_set_pixel_rgbw(led_state.strip, i,
                                 chan_out(c.red, level),
                                 chan_out(c.green, level),
                                 chan_out(c.blue, level),
                                 chan_out(c.white, level));
    }
    else
    {
        led_strip_set_pixel(led_state.strip, i,
                            chan_out(c.red, level),
                            chan_out(c.green, level),
                            chan_out(c.blue, level));
    }
}

static bool led_apply_color(const led_color_t *color)
{
    bool ok = false;

    strip_lock();
    if (led_state.strip && led_state.config_ready)
    {
        for (int i = 0; i < led_state.count; i++)
        {
            put_pixel(i, *color, 255);
        }

        esp_err_t err = led_strip_refresh(led_state.strip);
        if (err == ESP_OK)
        {
            led_state.last_color = *color;
            ok = true;
        }
        else
        {
            ESP_LOGE(TAG, "Failed to refresh LED strip: %s", esp_err_to_name(err));
        }
    }
    strip_unlock();

    return ok;
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

static void effect_fill(led_color_t c, uint8_t level)
{
    for (int i = 0; i < led_state.count; i++)
    {
        put_pixel(i, c, level);
    }
}

static uint32_t effect_period_ms(led_effect_t effect)
{
    switch (effect)
    {
        case LED_EFFECT_BREATHING: return BREATHING_PERIOD_MS;
        case LED_EFFECT_RAINBOW:   return RAINBOW_PERIOD_MS;
        case LED_EFFECT_FADE:      return FADE_PERIOD_MS;
        default:                   return 1000;
    }
}

// Renderiza um frame do efeito a partir do relógio, não de um contador de
// frames. NÃO mexe em last_color (a cor sólida fica preservada para quando o
// efeito for interrompido).
static void effect_render(led_effect_t effect, const led_color_t *base)
{
    uint32_t period = effect_period_ms(effect);
    float phase01 = (float)(now_ms() % period) / (float)period;

    strip_lock();
    if (!led_state.strip || !led_state.config_ready || led_state.count <= 0)
    {
        strip_unlock();
        return;
    }

    switch (effect)
    {
        case LED_EFFECT_BREATHING:
        {
            // Senoide no espaço perceptual: com o gamma aplicado depois, o
            // brilho *percebido* é que varia senoidalmente.
            float wave = (sinf(phase01 * 2.0f * (float)M_PI) + 1.0f) / 2.0f; // 0..1
            const uint8_t BREATHING_MIN = 6;
            uint8_t level = (uint8_t)(BREATHING_MIN + wave * (255 - BREATHING_MIN));
            effect_fill(*base, level);
            break;
        }
        case LED_EFFECT_RAINBOW:
        {
            uint8_t offset = (uint8_t)(phase01 * 256.0f);
            for (int i = 0; i < led_state.count; i++)
            {
                uint8_t h = (uint8_t)(offset + (i * 256) / led_state.count);
                led_color_t c = hsv_to_rgb(h, 255, 255);
                put_pixel(i, c, 255);
            }
            break;
        }
        case LED_EFFECT_FADE:
        {
            led_color_t c = hsv_to_rgb((uint8_t)(phase01 * 256.0f), 255, 255);
            effect_fill(c, 255);
            break;
        }
        default:
            strip_unlock();
            return;
    }

    led_strip_refresh(led_state.strip);
    strip_unlock();
}

// Orçamento de frame: o refresh de uma fita longa domina o tempo de ciclo
// (589 LEDs RGBW = 589 * 32 bits * 1,25 us ~= 23,6 ms). Pedir mais fps do que
// a fita comporta só satura o core 1 e faz o RMT (sem DMA no ESP32) disputar
// interrupções com o WiFi.
static uint32_t compute_frame_ms(int count, led_strip_type_t type)
{
    uint32_t bits = (type == LED_STRIP_TYPE_SK6812) ? 32u : 24u;
    uint32_t refresh_us = (uint32_t)(((uint64_t)count * bits * 5) / 4); // 1,25 us/bit
    uint32_t budget_ms = (refresh_us + (refresh_us / 2)) / 1000;        // 1,5x de folga
    return (budget_ms < LED_MIN_FRAME_MS) ? LED_MIN_FRAME_MS : budget_ms;
}

// Toda a animação vive aqui: a task fica bloqueada na fila quando ocioso e,
// quando um efeito está ativo, acorda mirando um deadline fixo por frame.
static void led_task(void *arg)
{
    led_msg_t msg;
    led_effect_t active = LED_EFFECT_NONE;
    led_color_t base = {255, 255, 255, 0};
    led_color_t solid = {0, 0, 0, 0};
    int64_t next_frame_us = 0;

    while (1)
    {
        TickType_t wait;
        if (active == LED_EFFECT_NONE)
        {
            wait = portMAX_DELAY;
        }
        else
        {
            // Arredonda para baixo: acordar um tick cedo e renderizar um pouco
            // antes do deadline é melhor do que passar dele. Como o deadline
            // avança sempre a partir do anterior, não há drift acumulado.
            int64_t delta_us = next_frame_us - esp_timer_get_time();
            wait = (delta_us <= 0) ? 0 : pdMS_TO_TICKS((uint32_t)(delta_us / 1000));
        }

        if (xQueueReceive(led_state.queue, &msg, wait) == pdTRUE)
        {
            switch (msg.type)
            {
                case LED_MSG_COLOR:
                    active = LED_EFFECT_NONE;
                    solid = msg.color;
                    led_apply_color(&msg.color);
                    break;

                case LED_MSG_EFFECT:
                    active = msg.effect;
                    base = msg.color;
                    next_frame_us = esp_timer_get_time();
                    if (active == LED_EFFECT_NONE)
                    {
                        led_apply_color(&solid); // restaura cor sólida
                    }
                    break;
            }
        }
        else if (active != LED_EFFECT_NONE)
        {
            // deadline atingido -> próximo frame do efeito
            effect_render(active, &base);

            int64_t now = esp_timer_get_time();
            next_frame_us += (int64_t)led_state.frame_ms * 1000;
            if (next_frame_us < now)
            {
                // renderização atrasou; não tenta recuperar frames perdidos
                next_frame_us = now + (int64_t)led_state.frame_ms * 1000;
            }
        }
    }
}

bool led_controller_start(void)
{
    if (led_state.strip_mutex == NULL)
    {
        led_state.strip_mutex = xSemaphoreCreateMutex();
        if (led_state.strip_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create LED strip mutex");
            return false;
        }
    }

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

    // Reconexão com a mesma configuração não deve derrubar a fita: além de
    // evitar o blink, tira a janela em que o strip é destruído sob a led_task.
    if (led_state.config_ready &&
        led_state.pin == led_pin &&
        led_state.count == led_count &&
        led_state.type == led_type)
    {
        ESP_LOGI(TAG, "LED config unchanged (pin=%d count=%d) - keeping strip alive", led_pin, led_count);
        return true;
    }

    strip_lock();
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
        strip_unlock();
        return false;
    }

    led_state.pin = led_pin;
    led_state.count = led_count;
    led_state.type = led_type;
    led_state.frame_ms = compute_frame_ms(led_count, led_type);
    led_state.config_ready = true;
    strip_unlock();

    ESP_LOGI(TAG, "LED strip configured: pin=%d, count=%d, type=%s, frame=%ums (~%u fps)",
             led_pin, led_count,
             (led_type == LED_STRIP_TYPE_SK6812) ? "sk6812" : "ws2812b",
             (unsigned)led_state.frame_ms,
             (unsigned)(1000 / led_state.frame_ms));

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
