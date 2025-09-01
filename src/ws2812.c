#include "ws2812.h"
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ws2812_module, LOG_LEVEL_INF);

static const struct device *strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixel[STRIP_NUM_PIXELS];

// Make colors global so main.c can use them
const struct led_rgb RED    = { .r = 0x0f, .g = 0x00, .b = 0x00 };
const struct led_rgb GREEN  = { .r = 0x00, .g = 0x0f, .b = 0x00 };
const struct led_rgb BLUE   = { .r = 0x00, .g = 0x00, .b = 0x0f };
const struct led_rgb VIOLET = { .r = 0x0f, .g = 0x00, .b = 0x0f };
const struct led_rgb OFF    = { .r = 0x00, .g = 0x00, .b = 0x00 };

static void update_strip(void) {
    int rc = led_strip_update_rgb(strip, pixel, STRIP_NUM_PIXELS);
    if (rc) LOG_ERR("LED update failed: %d", rc);
}

void ws2812_set_color(const struct led_rgb *color) {
     printk("Setting RGB = %02x %02x %02x\n", color->r, color->g, color->b);
    pixel[0] = *color;
//     pixel[0].r = 0x00;
//    pixel[0].g = 0x00;
//    pixel[0].b = 0x00;
    update_strip();
}

void ws2812_blink_color(const struct led_rgb *color, int times) {
    for (int i = 0; i < times; i++) {
        ws2812_set_color(color);
        k_sleep(DELAY_TIME);
        ws2812_set_color(&OFF);
        k_sleep(DELAY_TIME);
    }
}

void ws2812_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip device not ready");
        return;
    }
    else{
        printk("------------------------------enabled led-----------------\n");
       // ws2812_blink_red();
    }
    ws2812_set_color(&OFF);
}

void ws2812_blink_red(void) { ws2812_blink_color(&RED, 10); }
void ws2812_blink_blue(void) { ws2812_blink_color(&BLUE, 10); }
void ws2812_blink_violet(void) { ws2812_blink_color(&VIOLET, 10); }
void ws2812_blink_green(void) { ws2812_blink_color(&GREEN, 10); }