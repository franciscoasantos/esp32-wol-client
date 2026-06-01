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
} led_effect_t;

bool led_controller_start(void);
bool led_controller_configure(int led_pin, int led_count, led_strip_type_t led_type);
bool led_controller_enqueue(const led_color_t *color, int timeout_ms);
// Inicia/troca o efeito. base_color é a cor de referência (ex.: breathing).
// LED_EFFECT_NONE interrompe o efeito e restaura a última cor sólida.
bool led_controller_set_effect(led_effect_t effect, const led_color_t *base_color, int timeout_ms);
bool led_controller_is_configured(void);
led_color_t led_controller_get_current_color(void);

#endif
