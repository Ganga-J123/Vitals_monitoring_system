#ifndef BUTTON_H
#define BUTTON_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define LONG_PRESS_MS     1000   /* 1 second long press */
#define DOUBLE_PRESS_GAP  500    /* Max gap (ms) for double press */
#define DEBOUNCE_MS       50     /* Debounce time */

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SINGLE_PRESS,
    BUTTON_EVENT_DOUBLE_PRESS,
    BUTTON_EVENT_LONG_PRESS
} button_event_t;

int button_init(void);
//button_event_t button_get_event(void);

#endif /* BUTTON_H */
