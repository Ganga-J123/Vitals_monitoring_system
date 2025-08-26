#include "button.h"
#include "ws2812.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(button_driver, LOG_LEVEL_INF);

#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

/* State */
static int64_t press_start_time = 0;
static int64_t last_interrupt_time = 0;
static bool single_pending = false;

/* Timers & work */
static struct k_timer single_press_timer;
static struct k_work button_work;
static button_event_t pending_event = BUTTON_EVENT_NONE;

/* Forward declaration */
static void single_press_timeout(struct k_timer *timer_id);
static void button_work_handler(struct k_work *work);

static struct k_work_delayable led_off_work;

void led_off_handler(struct k_work *work)
{
    ws2812_set_color(&OFF);
}

/* ISR – lightweight only */
static void button_isr(const struct device *dev,
                       struct gpio_callback *cb,
                       uint32_t pins)
{
    int64_t now = k_uptime_get();

    /* Debounce */
    if (now - last_interrupt_time < DEBOUNCE_MS) {
        return;
    }
    last_interrupt_time = now;

    if (gpio_pin_get_dt(&button)) {
        /* Button pressed */
        press_start_time = now;
    } else {
        /* Button released */
        int64_t press_duration = now - press_start_time;

        if (press_duration >= LONG_PRESS_MS) {
            /* Long press overrides everything */
            k_timer_stop(&single_press_timer);
            single_pending = false;
            pending_event = BUTTON_EVENT_LONG_PRESS;
            k_work_submit(&button_work);

        } else {
            if (single_pending) {
                /* Second press → double */
                k_timer_stop(&single_press_timer);
                single_pending = false;
                pending_event = BUTTON_EVENT_DOUBLE_PRESS;
                k_work_submit(&button_work);

            } else {
                /* First short press → wait for possible double */
                single_pending = true;
                k_timer_start(&single_press_timer,
                              K_MSEC(DOUBLE_PRESS_GAP), K_NO_WAIT);
            }
        }
    }
}

/* Timer expiry → confirm single press */
static void single_press_timeout(struct k_timer *timer_id)
{
    if (single_pending) {
        single_pending = false;
        pending_event = BUTTON_EVENT_SINGLE_PRESS;
        k_work_submit(&button_work);
    }
}

/* Work handler → safe to update LEDs here */
static void button_work_handler(struct k_work *work)
{
    switch (pending_event) {
    case BUTTON_EVENT_SINGLE_PRESS:
         LOG_INF("Single press → LED GREEN");
         k_work_cancel_delayable(&led_off_work);
         ws2812_set_color(&GREEN);

    // Schedule turning OFF after 1 minute
    k_work_schedule(&led_off_work, K_MINUTES(1));
        break;

    case BUTTON_EVENT_DOUBLE_PRESS:
        LOG_INF("Double press → Blink BLUE");
        k_work_cancel_delayable(&led_off_work);
        ws2812_blink_blue();
        break;

    case BUTTON_EVENT_LONG_PRESS:
        LOG_INF("Long press → LED OFF");
        k_work_cancel_delayable(&led_off_work);
        ws2812_set_color(&OFF);
        break;

    default:
        break;
    }

    pending_event = BUTTON_EVENT_NONE;
}

int button_init(void)
{
    if (!device_is_ready(button.port)) {
        LOG_ERR("Button not ready");
        return -ENODEV;
    }

    gpio_pin_configure_dt(&button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);

    gpio_init_callback(&button_cb_data, button_isr, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);

    k_timer_init(&single_press_timer, single_press_timeout, NULL);
    k_work_init(&button_work, button_work_handler);

    k_work_init_delayable(&led_off_work, led_off_handler);

    LOG_INF("Button initialized");
    return 0;
}
