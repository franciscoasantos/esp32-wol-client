#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t white; // Só usado para SK6812 RGBW
} led_color_t;

typedef enum
{
    LED_STRIP_TYPE_WS2812B = 0,
    LED_STRIP_TYPE_SK6812,
} led_strip_type_t;

// Efeitos rodam no próprio firmware (animação não-bloqueante na led_task).
typedef enum
{
    LED_EFFECT_NONE = 0,
    LED_EFFECT_BREATHING,
    LED_EFFECT_RAINBOW,
    LED_EFFECT_FADE,
    LED_EFFECT_FIRE,
    LED_EFFECT_COMET,
    LED_EFFECT_TWINKLE,
    LED_EFFECT_WAVE,
    LED_EFFECT_WIPE,
} led_effect_t;

// Valor que diz "usar o padrão deste efeito". Mantém o look atual de quem não
// manda o parâmetro, já que o padrão bom varia por efeito.
#define LED_PARAM_DEFAULT 255

typedef struct
{
    uint8_t speed;      // 0..100 (maior = mais rápido) ou LED_PARAM_DEFAULT
    uint8_t intensity;  // 0..100 (significado por efeito) ou LED_PARAM_DEFAULT
} led_effect_params_t;

// ---------------------------------------------------------------------------
// Padrões estáticos: o que a fita mostra quando não há efeito rodando. Em vez
// de guardar um buffer de pixels, guardamos a descrição e avaliamos por pixel
// na hora de renderizar — bem mais barato em RAM para 589 LEDs, e o crossfade
// só precisa avaliar os dois padrões.
// ---------------------------------------------------------------------------

#define LED_MAX_STOPS    8
#define LED_MAX_SEGMENTS 8

typedef enum
{
    LED_PATTERN_SOLID = 0,
    LED_PATTERN_GRADIENT,
    LED_PATTERN_SEGMENTS,
} led_pattern_type_t;

typedef struct
{
    uint8_t pos;        // 0..255 ao longo da fita
    led_color_t color;
} led_stop_t;

typedef struct
{
    uint16_t from;      // índices inclusivos
    uint16_t to;
    led_color_t color;
} led_segment_t;

typedef struct
{
    led_pattern_type_t type;
    led_color_t solid;                          // LED_PATTERN_SOLID
    uint8_t stop_count;
    led_stop_t stops[LED_MAX_STOPS];            // LED_PATTERN_GRADIENT
    uint8_t segment_count;
    led_segment_t segments[LED_MAX_SEGMENTS];   // LED_PATTERN_SEGMENTS
} led_pattern_t;

bool led_controller_start(void);
bool led_controller_configure(int led_pin, int led_count, led_strip_type_t led_type);
// Reaplica a última configuração e cor salvas na NVS. Feito antes da rede para
// a fita acender no boot sem esperar WiFi + TLS + get_config.
bool led_controller_restore(void);
bool led_controller_enqueue(const led_color_t *color, int timeout_ms);
// Igual ao enqueue, mas interpolando do padrão atual até `color` ao longo de
// fade_ms. fade_ms = 0 aplica na hora.
bool led_controller_enqueue_fade(const led_color_t *color, uint16_t fade_ms, int timeout_ms);
// Aplica um padrão (sólido, gradiente ou segmentos), com o mesmo crossfade.
bool led_controller_set_pattern(const led_pattern_t *pattern, uint16_t fade_ms, int timeout_ms);
// Inicia/troca o efeito. base_color é a cor de referência (ex.: breathing) e
// params pode ser NULL para usar os padrões de cada efeito.
// LED_EFFECT_NONE interrompe o efeito e restaura a última cor sólida.
bool led_controller_set_effect(led_effect_t effect,
                               const led_color_t *base_color,
                               const led_effect_params_t *params,
                               int timeout_ms);
bool led_controller_is_configured(void);
led_color_t led_controller_get_current_color(void);

#endif
