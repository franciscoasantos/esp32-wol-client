#include "led_controller.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "led_strip.h"

static const char *TAG = "led_controller";

#define LED_QUEUE_LENGTH 8
#define LED_MIN_FRAME_MS 20   // teto de ~50 fps para fitas curtas

// A cor só vai para a NVS depois de ficar parada por este tempo. Um arraste no
// seletor manda uma cor a cada 140 ms; sem isso seriam ~7 escritas de flash/s.
#define LED_SAVE_DEBOUNCE_MS 5000

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Períodos de animação em tempo de parede. Antes a velocidade vinha de um
// incremento por frame, o que fazia o mesmo efeito rodar mais devagar em fitas
// longas (o refresh de 589 LEDs RGBW leva ~23,6 ms e entrava no orçamento).
#define BREATHING_PERIOD_MS 6800
#define RAINBOW_PERIOD_MS   3000
#define FADE_PERIOD_MS      6000
#define FIRE_PERIOD_MS      1000   // o fogo é iterativo; o período só regula o passo
#define COMET_PERIOD_MS     2000
#define TWINKLE_PERIOD_MS   4000
#define WAVE_PERIOD_MS      5000
#define WIPE_PERIOD_MS      3000

#define NVS_NS        "espnest_led"
#define NVS_KEY_PIN   "pin"
#define NVS_KEY_COUNT "count"
#define NVS_KEY_TYPE  "type"
#define NVS_KEY_PATTERN "pattern"

// Mensagens enviadas para a led_task.
typedef enum {
    LED_MSG_PATTERN = 0,
    LED_MSG_EFFECT
} led_msg_type_t;

typedef struct {
    led_msg_type_t type;
    led_pattern_t pattern;  // usado quando type == LED_MSG_PATTERN
    led_color_t color;      // cor base do efeito (EFFECT)
    led_effect_t effect;    // usado quando type == LED_MSG_EFFECT
    led_effect_params_t params; // idem
    uint16_t fade_ms;       // usado quando type == LED_MSG_PATTERN (0 = imediato)
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
    uint8_t *heat;           // mapa de calor do efeito fogo (1 byte por LED)
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
    .frame_ms = LED_MIN_FRAME_MS,
    .heat = NULL
};

// Seno 0..255 indexado por ângulo 0..255, montado no start(). Fica em RAM (256
// bytes) em vez de na flash, que é o recurso apertado neste projeto.
static uint8_t SIN8[256];

static void build_sin_table(void)
{
    for (int i = 0; i < 256; i++)
    {
        float a = (float)i / 256.0f * 2.0f * (float)M_PI;
        SIN8[i] = (uint8_t)((sinf(a) + 1.0f) * 127.5f);
    }
}

// xorshift32: o inner loop do fogo e do twinkle chama isto por pixel, então
// vale evitar o custo do esp_random() a cada chamada.
static uint32_t rng_state = 0x2545F491u;

static inline uint32_t fast_rand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static inline uint8_t qsub8(uint8_t a, uint8_t b) { return (a > b) ? (uint8_t)(a - b) : 0; }
static inline uint8_t qadd8(uint8_t a, uint8_t b) { uint16_t s = (uint16_t)a + b; return (s > 255) ? 255 : (uint8_t)s; }

// Resolve um parâmetro 0..100, caindo no padrão do efeito quando não veio.
static inline uint8_t param_or(uint8_t value, uint8_t fallback)
{
    return (value > 100) ? fallback : value;
}

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

static inline bool color_equals(const led_color_t *a, const led_color_t *b)
{
    return a->red == b->red && a->green == b->green && a->blue == b->blue && a->white == b->white;
}

// ===================== PERSISTÊNCIA (NVS) =====================

static void nvs_save_config(int pin, int count, led_strip_type_t type)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS open failed; config not persisted");
        return;
    }
    nvs_set_i32(h, NVS_KEY_PIN, pin);
    nvs_set_i32(h, NVS_KEY_COUNT, count);
    nvs_set_u8(h, NVS_KEY_TYPE, (uint8_t)type);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_pattern(const led_pattern_t *pattern)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS open failed; pattern not persisted");
        return;
    }
    nvs_set_blob(h, NVS_KEY_PATTERN, pattern, sizeof(*pattern));
    nvs_commit(h);
    nvs_close(h);
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

// ===================== PADRÕES ESTÁTICOS =====================

static inline uint8_t lerp8(uint8_t a, uint8_t b, uint32_t num, uint32_t den)
{
    if (den == 0) return b;
    int32_t delta = (int32_t)b - (int32_t)a;
    return (uint8_t)((int32_t)a + (delta * (int32_t)num) / (int32_t)den);
}

// Interpola no espaço bruto (pré-gamma), que é onde as cores são
// perceptualmente uniformes — o gamma é aplicado depois, no funil de saída.
static led_color_t color_lerp(led_color_t a, led_color_t b, uint32_t num, uint32_t den)
{
    led_color_t c;
    c.red   = lerp8(a.red,   b.red,   num, den);
    c.green = lerp8(a.green, b.green, num, den);
    c.blue  = lerp8(a.blue,  b.blue,  num, den);
    c.white = lerp8(a.white, b.white, num, den);
    return c;
}

static void pattern_make_solid(led_pattern_t *pattern, const led_color_t *color)
{
    memset(pattern, 0, sizeof(*pattern));
    pattern->type = LED_PATTERN_SOLID;
    pattern->solid = *color;
}

// Cor do padrão no pixel `i`. Roda uma vez por pixel por frame; o laço de
// stops é O(8) no pior caso, o que some perto do custo do refresh.
static led_color_t pattern_color_at(const led_pattern_t *pattern, int i, int count)
{
    const led_color_t black = {0, 0, 0, 0};

    switch (pattern->type)
    {
        case LED_PATTERN_GRADIENT:
        {
            if (pattern->stop_count == 0) return black;
            if (pattern->stop_count == 1) return pattern->stops[0].color;

            uint8_t pos = (count > 1)
                ? (uint8_t)(((uint32_t)i * 255u) / (uint32_t)(count - 1))
                : 0;

            if (pos <= pattern->stops[0].pos) return pattern->stops[0].color;

            for (int s = 0; s < pattern->stop_count - 1; s++)
            {
                uint8_t a = pattern->stops[s].pos;
                uint8_t b = pattern->stops[s + 1].pos;
                if (pos <= b)
                {
                    uint32_t den = (b > a) ? (uint32_t)(b - a) : 1u;
                    uint32_t num = (pos > a) ? (uint32_t)(pos - a) : 0u;
                    return color_lerp(pattern->stops[s].color, pattern->stops[s + 1].color, num, den);
                }
            }
            return pattern->stops[pattern->stop_count - 1].color;
        }

        case LED_PATTERN_SEGMENTS:
        {
            for (int s = 0; s < pattern->segment_count; s++)
            {
                if (i >= (int)pattern->segments[s].from && i <= (int)pattern->segments[s].to)
                {
                    return pattern->segments[s].color;
                }
            }
            return black; // pixel fora de qualquer segmento fica apagado
        }

        case LED_PATTERN_SOLID:
        default:
            return pattern->solid;
    }
}

// Renderiza um padrão, ou a interpolação entre dois quando `from` não é NULL.
static bool apply_pattern(const led_pattern_t *from, const led_pattern_t *to, uint32_t num, uint32_t den)
{
    bool ok = false;

    strip_lock();
    if (led_state.strip && led_state.config_ready)
    {
        for (int i = 0; i < led_state.count; i++)
        {
            led_color_t c = pattern_color_at(to, i, led_state.count);
            if (from)
            {
                c = color_lerp(pattern_color_at(from, i, led_state.count), c, num, den);
            }
            put_pixel(i, c, 255);
        }

        esp_err_t err = led_strip_refresh(led_state.strip);
        if (err == ESP_OK)
        {
            // Cor representativa do padrão: é o que vai no state_report, já
            // que o protocolo de estado ainda fala em uma cor só.
            led_state.last_color = pattern_color_at(to, 0, led_state.count);
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

static bool led_apply_color(const led_color_t *color)
{
    led_pattern_t pattern;
    pattern_make_solid(&pattern, color);
    return apply_pattern(NULL, &pattern, 0, 0);
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

static uint32_t effect_base_period_ms(led_effect_t effect)
{
    switch (effect)
    {
        case LED_EFFECT_BREATHING: return BREATHING_PERIOD_MS;
        case LED_EFFECT_RAINBOW:   return RAINBOW_PERIOD_MS;
        case LED_EFFECT_FADE:      return FADE_PERIOD_MS;
        case LED_EFFECT_FIRE:      return FIRE_PERIOD_MS;
        case LED_EFFECT_COMET:     return COMET_PERIOD_MS;
        case LED_EFFECT_TWINKLE:   return TWINKLE_PERIOD_MS;
        case LED_EFFECT_WAVE:      return WAVE_PERIOD_MS;
        case LED_EFFECT_WIPE:      return WIPE_PERIOD_MS;
        default:                   return 1000;
    }
}

// speed 0..100 -> período de 4x (lento) a 1/4x (rápido); 50 mantém o padrão.
static uint32_t scale_period(uint32_t base_ms, uint8_t speed)
{
    uint32_t pct = (speed <= 50)
        ? (400u - (uint32_t)speed * 6u)
        : (100u - ((uint32_t)(speed - 50) * 3u) / 2u);
    if (pct < 5) pct = 5;
    uint32_t scaled = (base_ms * pct) / 100u;
    return (scaled < 50) ? 50 : scaled;
}

// Intensidade padrão por efeito: o que faz cada um parecer "certo" sem ajuste.
static uint8_t effect_default_intensity(led_effect_t effect)
{
    switch (effect)
    {
        case LED_EFFECT_BREATHING: return 100; // profundidade cheia (look original)
        case LED_EFFECT_FIRE:      return 55;
        case LED_EFFECT_COMET:     return 50;  // cauda ~30% da fita
        case LED_EFFECT_TWINKLE:   return 40;  // densidade de estrelas
        case LED_EFFECT_WAVE:      return 60;  // número de cristas
        case LED_EFFECT_WIPE:      return 50;  // suavidade da borda
        default:                   return 50;
    }
}

// Fogo estilo Fire2012: esfria, difunde para cima e solta faíscas na base.
// É o único efeito com estado entre frames (o mapa de calor).
static void render_fire(uint8_t intensity)
{
    const int n = led_state.count;
    uint8_t *heat = led_state.heat;
    if (!heat || n <= 0) return;

    // Fita longa dissipa mais devagar por LED, senão a chama nunca sobe.
    uint8_t cooling = (uint8_t)(((55u + (100u - intensity)) * 10u) / (uint32_t)(n > 0 ? n : 1) + 2u);
    uint8_t sparking = (uint8_t)(60 + (intensity * 145) / 100);

    for (int i = 0; i < n; i++)
    {
        heat[i] = qsub8(heat[i], (uint8_t)(fast_rand() % (cooling + 1u)));
    }

    for (int k = n - 1; k >= 2; k--)
    {
        heat[k] = (uint8_t)(((uint16_t)heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3);
    }

    if ((fast_rand() % 255u) < sparking)
    {
        int y = (int)(fast_rand() % 7u);
        if (y < n) heat[y] = qadd8(heat[y], (uint8_t)(160 + fast_rand() % 96u));
    }

    // Rampa preto -> vermelho -> amarelo -> branco.
    for (int i = 0; i < n; i++)
    {
        uint8_t h = heat[i];
        uint8_t t192 = (uint8_t)(((uint16_t)h * 191) / 255);
        uint8_t heatramp = (uint8_t)((t192 & 0x3F) << 2);
        led_color_t c = {0, 0, 0, 0};
        if (t192 > 128)      { c.red = 255; c.green = 255; c.blue = heatramp; }
        else if (t192 > 64)  { c.red = 255; c.green = heatramp; }
        else                 { c.red = heatramp; }
        put_pixel(i, c, 255);
    }
}

// Cabeça deslizando com cauda que decai; sem estado entre frames.
static void render_comet(const led_color_t *base, float phase01, uint8_t intensity)
{
    const int n = led_state.count;
    int tail = (int)((uint32_t)n * (5u + intensity / 2u) / 100u);
    if (tail < 1) tail = 1;

    float head = phase01 * (float)n;

    for (int i = 0; i < n; i++)
    {
        float d = head - (float)i;
        if (d < 0) d += (float)n;
        uint8_t level = 0;
        if (d < (float)tail)
        {
            level = (uint8_t)(255.0f * (1.0f - d / (float)tail));
        }
        put_pixel(i, *base, level);
    }
}

// Estrelas piscando sobre um piso fraco. Sem buffer: quem acende e quando sai
// de um hash de (índice, janela de tempo).
static void render_twinkle(const led_color_t *base, uint32_t period, uint8_t intensity)
{
    const int n = led_state.count;
    const uint8_t FLOOR_LEVEL = 10;
    uint32_t t = now_ms();
    uint32_t slot = t / period;
    uint32_t density = 5u + ((uint32_t)intensity * 45u) / 100u; // 5%..50% dos LEDs

    for (int i = 0; i < n; i++)
    {
        uint32_t h = hash32((uint32_t)i * 2654435761u ^ slot);
        uint8_t level = FLOOR_LEVEL;

        if ((h % 100u) < density)
        {
            uint32_t offset = (h >> 8) % period;
            uint32_t frac = ((t + offset) % period) * 255u / period;
            // Pulso triangular: sobe até a metade da janela e volta.
            uint8_t pulse = (frac < 128) ? (uint8_t)(frac * 2) : (uint8_t)((255 - frac) * 2);
            if (pulse > FLOOR_LEVEL) level = pulse;
        }

        put_pixel(i, *base, level);
    }
}

// Duas senoides defasadas somadas; o clássico "plasma" barato.
static void render_wave(const led_color_t *base, float phase01, uint8_t intensity)
{
    const int n = led_state.count;
    uint32_t crests = 1u + ((uint32_t)intensity * 5u) / 100u;
    uint8_t p1 = (uint8_t)(phase01 * 256.0f);
    uint8_t p2 = (uint8_t)(phase01 * 179.0f); // ~0.7x, para as ondas não baterem

    for (int i = 0; i < n; i++)
    {
        uint32_t spatial = ((uint32_t)i * 256u * crests) / (uint32_t)n;
        uint8_t a = SIN8[(uint8_t)(p1 + spatial)];
        uint8_t b = SIN8[(uint8_t)(p2 + (spatial * 3u) / 5u + 77u)];
        put_pixel(i, *base, (uint8_t)(((uint16_t)a + b) / 2));
    }
}

// Preenche progressivamente e recomeça; a borda tem um degradê de saída.
static void render_wipe(const led_color_t *base, float phase01, uint8_t intensity)
{
    const int n = led_state.count;
    int feather = (int)((uint32_t)n * (1u + intensity / 5u) / 100u);
    if (feather < 1) feather = 1;

    int filled = (int)(phase01 * (float)(n + feather));

    for (int i = 0; i < n; i++)
    {
        uint8_t level;
        // Atrás da borda já está cheio; na borda vai de 0 (cabeça) a 255.
        if (i < filled - feather)      level = 255;
        else if (i > filled)           level = 0;
        else                           level = (uint8_t)(((filled - i) * 255) / feather);
        put_pixel(i, *base, level);
    }
}

// Renderiza um frame do efeito a partir do relógio, não de um contador de
// frames. NÃO mexe em last_color (a cor sólida fica preservada para quando o
// efeito for interrompido).
static void effect_render(led_effect_t effect, const led_color_t *base, const led_effect_params_t *params)
{
    uint8_t speed = param_or(params->speed, 50);
    uint8_t intensity = param_or(params->intensity, effect_default_intensity(effect));

    uint32_t period = scale_period(effect_base_period_ms(effect), speed);
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
            // intensity = profundidade: 100 chega quase a apagar, 0 quase não pulsa.
            uint8_t floor_level = (uint8_t)(255 - ((uint32_t)intensity * 249u) / 100u);
            uint8_t level = (uint8_t)(floor_level + wave * (255 - floor_level));
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
        case LED_EFFECT_FIRE:
            render_fire(intensity);
            break;
        case LED_EFFECT_COMET:
            render_comet(base, phase01, intensity);
            break;
        case LED_EFFECT_TWINKLE:
            render_twinkle(base, period, intensity);
            break;
        case LED_EFFECT_WAVE:
            render_wave(base, phase01, intensity);
            break;
        case LED_EFFECT_WIPE:
            render_wipe(base, phase01, intensity);
            break;
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

// Toda a animação vive aqui. A task fica bloqueada na fila quando não há nada
// acontecendo e acorda mirando um deadline — de frame (efeito ou crossfade) ou
// de gravação na NVS.
static void led_task(void *arg)
{
    led_msg_t msg;
    led_effect_t active = LED_EFFECT_NONE;
    led_color_t base = {255, 255, 255, 0};
    led_effect_params_t params = { LED_PARAM_DEFAULT, LED_PARAM_DEFAULT };
    int64_t next_frame_us = 0;

    // Padrão atual da fita quando nenhum efeito está rodando.
    static led_pattern_t current;
    static led_pattern_t fade_from;
    static led_pattern_t saved;
    const led_color_t off = {0, 0, 0, 0};
    pattern_make_solid(&current, &off);

    bool fading = false;
    int64_t fade_start_us = 0;
    uint32_t fade_ms = 0;

    bool save_pending = false;
    int64_t save_deadline_us = 0;
    bool saved_valid = false;

    while (1)
    {
        int64_t now = esp_timer_get_time();
        TickType_t wait;

        if (active != LED_EFFECT_NONE || fading)
        {
            // Arredonda para baixo: acordar um tick cedo e renderizar um pouco
            // antes do deadline é melhor do que passar dele. Como o deadline
            // avança sempre a partir do anterior, não há drift acumulado.
            int64_t delta_us = next_frame_us - now;
            wait = (delta_us <= 0) ? 0 : pdMS_TO_TICKS((uint32_t)(delta_us / 1000));
        }
        else if (save_pending)
        {
            int64_t delta_us = save_deadline_us - now;
            wait = (delta_us <= 0) ? 0 : pdMS_TO_TICKS((uint32_t)(delta_us / 1000));
        }
        else
        {
            wait = portMAX_DELAY;
        }

        if (xQueueReceive(led_state.queue, &msg, wait) == pdTRUE)
        {
            switch (msg.type)
            {
                case LED_MSG_PATTERN:
                    active = LED_EFFECT_NONE;
                    if (msg.fade_ms == 0)
                    {
                        fading = false;
                        current = msg.pattern;
                        apply_pattern(NULL, &current, 0, 0);
                    }
                    else
                    {
                        // A transição sempre parte do último padrão estático.
                        // Se um efeito estava rodando, o que está na fita é o
                        // frame do efeito, então há um salto antes do fade.
                        fade_from = current;
                        current = msg.pattern;
                        fade_ms = msg.fade_ms;
                        fade_start_us = esp_timer_get_time();
                        next_frame_us = fade_start_us;
                        fading = true;
                    }
                    save_pending = true;
                    save_deadline_us = esp_timer_get_time() + (int64_t)LED_SAVE_DEBOUNCE_MS * 1000;
                    break;

                case LED_MSG_EFFECT:
                    active = msg.effect;
                    base = msg.color;
                    params = msg.params;
                    fading = false;
                    if (active == LED_EFFECT_FIRE && led_state.heat)
                    {
                        // Chama sempre começa fria, não de onde parou.
                        strip_lock();
                        memset(led_state.heat, 0, (size_t)led_state.count);
                        strip_unlock();
                    }
                    next_frame_us = esp_timer_get_time();
                    if (active == LED_EFFECT_NONE)
                    {
                        apply_pattern(NULL, &current, 0, 0); // restaura o padrão
                    }
                    break;
            }
            continue;
        }

        // Nada na fila: o que venceu foi um deadline.
        now = esp_timer_get_time();

        if (fading)
        {
            uint32_t elapsed = (uint32_t)((now - fade_start_us) / 1000);
            if (elapsed >= fade_ms)
            {
                fading = false;
                apply_pattern(NULL, &current, 0, 0);
            }
            else
            {
                apply_pattern(&fade_from, &current, elapsed, fade_ms);
                next_frame_us += (int64_t)led_state.frame_ms * 1000;
                if (next_frame_us < now)
                {
                    next_frame_us = now + (int64_t)led_state.frame_ms * 1000;
                }
            }
        }
        else if (active != LED_EFFECT_NONE)
        {
            effect_render(active, &base, &params);

            next_frame_us += (int64_t)led_state.frame_ms * 1000;
            if (next_frame_us < now)
            {
                // renderização atrasou; não tenta recuperar frames perdidos
                next_frame_us = now + (int64_t)led_state.frame_ms * 1000;
            }
        }
        else if (save_pending)
        {
            save_pending = false;
            if (!saved_valid || memcmp(&saved, &current, sizeof(current)) != 0)
            {
                nvs_save_pattern(&current);
                saved = current;
                saved_valid = true;
                ESP_LOGI(TAG, "Pattern persisted to NVS (type=%d)", (int)current.type);
            }
        }
    }
}

bool led_controller_start(void)
{
    build_sin_table();
    rng_state = esp_random() | 1u; // xorshift nao pode partir de zero

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

static bool configure_internal(int led_pin, int led_count, led_strip_type_t led_type, bool persist)
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

    free(led_state.heat);
    led_state.heat = calloc((size_t)led_count, sizeof(uint8_t));
    if (led_state.heat == NULL)
    {
        ESP_LOGW(TAG, "No heap for fire heat map (%d bytes); fire effect disabled", led_count);
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

    if (persist)
    {
        nvs_save_config(led_pin, led_count, led_type);
    }

    ESP_LOGI(TAG, "LED strip configured: pin=%d, count=%d, type=%s, frame=%ums (~%u fps)",
             led_pin, led_count,
             (led_type == LED_STRIP_TYPE_SK6812) ? "sk6812" : "ws2812b",
             (unsigned)led_state.frame_ms,
             (unsigned)(1000 / led_state.frame_ms));

    led_color_t off = {0};
    led_apply_color(&off);

    return true;
}

bool led_controller_configure(int led_pin, int led_count, led_strip_type_t led_type)
{
    return configure_internal(led_pin, led_count, led_type, true);
}

bool led_controller_restore(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return false; // primeira execução: ainda não há nada gravado
    }

    int32_t pin = -1;
    int32_t count = 0;
    uint8_t type = LED_STRIP_TYPE_WS2812B;
    esp_err_t pin_err = nvs_get_i32(h, NVS_KEY_PIN, &pin);
    esp_err_t count_err = nvs_get_i32(h, NVS_KEY_COUNT, &count);
    nvs_get_u8(h, NVS_KEY_TYPE, &type);

    static led_pattern_t pattern;
    size_t pattern_len = sizeof(pattern);
    bool has_pattern = (nvs_get_blob(h, NVS_KEY_PATTERN, &pattern, &pattern_len) == ESP_OK &&
                        pattern_len == sizeof(pattern));
    nvs_close(h);

    if (pin_err != ESP_OK || count_err != ESP_OK || pin < 0 || count <= 0)
    {
        return false;
    }

    if (type != LED_STRIP_TYPE_SK6812)
    {
        type = LED_STRIP_TYPE_WS2812B;
    }

    // persist = false: acabou de vir da NVS, não faz sentido regravar.
    if (!configure_internal((int)pin, (int)count, (led_strip_type_t)type, false))
    {
        return false;
    }

    if (has_pattern)
    {
        led_controller_set_pattern(&pattern, 0, 100);
    }

    ESP_LOGI(TAG, "Restored from NVS: pin=%d count=%d type=%s pattern=%s",
             (int)pin, (int)count,
             (type == LED_STRIP_TYPE_SK6812) ? "sk6812" : "ws2812b",
             has_pattern ? "yes" : "no");
    return true;
}

bool led_controller_enqueue(const led_color_t *color, int timeout_ms)
{
    return led_controller_enqueue_fade(color, 0, timeout_ms);
}

bool led_controller_enqueue_fade(const led_color_t *color, uint16_t fade_ms, int timeout_ms)
{
    if (color == NULL) return false;

    led_pattern_t pattern;
    pattern_make_solid(&pattern, color);
    return led_controller_set_pattern(&pattern, fade_ms, timeout_ms);
}

bool led_controller_set_pattern(const led_pattern_t *pattern, uint16_t fade_ms, int timeout_ms)
{
    if (led_state.queue == NULL || pattern == NULL)
    {
        return false;
    }

    led_msg_t msg = {
        .type = LED_MSG_PATTERN,
        .pattern = *pattern,
        .color = {0},
        .effect = LED_EFFECT_NONE,
        .params = { LED_PARAM_DEFAULT, LED_PARAM_DEFAULT },
        .fade_ms = fade_ms
    };
    if (xQueueSend(led_state.queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return false;
    }

    return true;
}

bool led_controller_set_effect(led_effect_t effect,
                               const led_color_t *base_color,
                               const led_effect_params_t *params,
                               int timeout_ms)
{
    if (led_state.queue == NULL)
    {
        return false;
    }

    led_msg_t msg = {
        .type = LED_MSG_EFFECT,
        .pattern = {0},
        .color = {255, 255, 255, 0},
        .effect = effect,
        .params = { LED_PARAM_DEFAULT, LED_PARAM_DEFAULT },
        .fade_ms = 0
    };
    if (base_color != NULL)
    {
        msg.color = *base_color;
    }
    if (params != NULL)
    {
        msg.params = *params;
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
