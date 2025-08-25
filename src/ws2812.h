#pragma once
#include <zephyr/drivers/led_strip.h>

struct led_rgb;

void ws2812_init(void);
void ws2812_set_color(const struct led_rgb *color);
void ws2812_blink_color(const struct led_rgb *color, int times);

void ws2812_blink_red(void);
void ws2812_blink_blue(void);
void ws2812_blink_violet(void);
void ws2812_blink_green(void);

// Expose colors for main.c
extern const struct led_rgb RED;
extern const struct led_rgb GREEN;
extern const struct led_rgb BLUE;
extern const struct led_rgb VIOLET;
extern const struct led_rgb OFF;
