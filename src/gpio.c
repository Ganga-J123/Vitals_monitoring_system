#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "gpio.h"

const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

void gpio_high(void)
{
      if (!device_is_ready(gpio0_dev)) {
        printk("Error: GPIO device not ready\n");
        return;
    }

    // Configure pin as output and set high
    int x=gpio_pin_configure(gpio0_dev, GPIO_PIN, GPIO_OUTPUT_ACTIVE);

    if(!x){printk("GPIO 16 set HIGH\n");}
    else{printk("gpio fail\n");}
} 