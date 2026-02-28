#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_color_t;

bool led_controller_start(void);
bool led_controller_configure(int led_pin, int led_count);
bool led_controller_enqueue(const led_color_t *color, int timeout_ms);
bool led_controller_is_configured(void);

#endif
