#pragma once
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define GPIO_PIN 16
#define GPIO0_NODE DT_NODELABEL(gpio0)
extern const struct device *gpio0_dev;

void gpio_high(void);