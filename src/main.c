#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <string.h>
#include <stdio.h>
#include <zephyr/types.h>
#include <time.h>
#include "adpd144ri.h"
#include "rtc.h"
#include "ble.h"
#include "temp.h"
#include "lis3dh.h"
#include "ws2812.h"
#include "button.h"
#include "gpio.h"


/* Thread objects */
static struct k_thread sensor_tid_data;
static struct k_thread printer_tid_data;
static struct k_thread init_tid_data;

/* Thread stacks */
#define SENSOR_STACK_SIZE 1280
#define PRINTER_STACK_SIZE 2560
#define INIT_STACK_SIZE 512

K_THREAD_STACK_DEFINE(sensor_stack, SENSOR_STACK_SIZE);
K_THREAD_STACK_DEFINE(printer_stack, PRINTER_STACK_SIZE);
K_THREAD_STACK_DEFINE(init_stack, INIT_STACK_SIZE);

/* Thread priorities */
#define SENSOR_PRIO 10
#define PRINTER_PRIO 9
#define INIT_PRIO 5


#define I2C_NODE DT_NODELABEL(i2c0)

#define GPIO_PIN 16

#define BATCH_SIZE 6
#define MSGQ_SIZE 20

#define MAX_SAMPLES 1998

#define I2C_MUTEX_TIMEOUT  K_MSEC(15)  // Max wait time for I2C access

static uint16_t a_cnt=0, p_cnt=0;

K_MUTEX_DEFINE(i2c_bus_mutex);

K_MSGQ_DEFINE(ppg_msgq, sizeof(PPGSample), MSGQ_SIZE * 2, 4);
K_MSGQ_DEFINE(temp_msgq, sizeof(TempSample), MSGQ_SIZE, 4);
K_MSGQ_DEFINE(accel_msgq, sizeof(lis3dh_data_t), MSGQ_SIZE * 2, 4);

extern struct k_mutex notify_buf_mutex;
extern volatile bool notify_enabled_ppg;
extern volatile bool notify_enabled_temp;
extern volatile bool notify_enabled_accel;

extern struct bt_conn *current_conn;
extern const struct bt_gatt_service_static sensor_svc;
extern const struct bt_gatt_attr *ppg_char_attr;
extern const struct bt_gatt_attr *temp_char_attr;
extern const struct bt_gatt_attr *accel_char_attr;

static lis3dh_sensor_t lis3dh_dev;

extern struct k_work ble_disconnect_work;
extern void ble_disconnect_work_handler(struct k_work *work);

// Custom timegm() replacement (UTC time -> timestamp)
time_t custom_timegm(struct tm *tm) {
    static const int days_in_month[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;  // 0-11
    int day = tm->tm_mday;

    // Count leap years
    int y = year - (month < 2);
    int leap_days = y / 4 - y / 100 + y / 400;

    // Days since epoch
    int days = (year - 1970) * 365 + leap_days;
    for (int i = 0; i < month; i++) {
        days += days_in_month[i];
    }

    // Add one more day for leap year if after Feb
    if (month > 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        days += 1;
    }

    days += day - 1;

    // Return seconds since epoch
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

uint64_t parse_timestamp_ms(const char *timestamp_str) {
    struct tm t = {0};
    int ms = 0;

    // Parse: "dd/mm/yyyy hh:mm:ss.mmm"
    sscanf(timestamp_str, "%d/%d/%d %d:%d:%d.%d",
           &t.tm_mday, &t.tm_mon, &t.tm_year,
           &t.tm_hour, &t.tm_min, &t.tm_sec, &ms);

    t.tm_mon -= 1;
    t.tm_year -= 1900;

    time_t seconds = custom_timegm(&t);
    return ((uint64_t)seconds * 1000) + ms;
}


void sensor_thread(void *p1, void *p2, void *p3)
{
    const struct device *i2c_dev = DEVICE_DT_GET(I2C_NODE);

    printk("[THREAD] sensor_thread started\n");

    if (!device_is_ready(i2c_dev)) {
        printk("[I2C] device %s not ready!\n", i2c_dev->name);
        return;
    }

    /* sensor inits (same as before) */
    if (k_mutex_lock(&i2c_bus_mutex, I2C_MUTEX_TIMEOUT) == 0) {
        if (!lis3dh_init(&lis3dh_dev, i2c_dev, 0x18)) {
            printk("[LIS3DH] Accelerometer sensor init failed\n");
        }
        k_mutex_unlock(&i2c_bus_mutex);
    }
    if (k_mutex_lock(&i2c_bus_mutex, I2C_MUTEX_TIMEOUT) == 0) {
        if (adpd144ri_configure_spo2_hr(i2c_dev) != 0) {
            printk("[PPG] PPG Sensor init failed\n");
        }
        k_mutex_unlock(&i2c_bus_mutex);
    }
    if (k_mutex_lock(&i2c_bus_mutex, I2C_MUTEX_TIMEOUT) == 0) {
        if (!lis3dh_init(&lis3dh_dev, i2c_dev, 0x18)) {
            printk("[LIS3DH] Accelerometer re-init failed\n");
        }
        k_mutex_unlock(&i2c_bus_mutex);
    }
    if (temp_sensor_init() != 0) {
        printk("[TEMP] Temperature sensor initialization failed\n");
    }
  //  rtc2_init();

    PPGSample ppg_sample;
    lis3dh_data_t accel_sample;
    TempSample temp_sample;

    static uint32_t ppg_drops = 0, accel_drops = 0;
    uint64_t last_ppg = 0, last_accel = 0;

    /* Wait here until notify is enabled */
    while (!ack_notify_enabled) {
        k_msleep(50);   // Sleep a bit to avoid busy loop
    }

    /* Once enabled, send the ack */
    send_ack_to_mobile("2#0");

    while (1) {
        /* Wait until any notify is enabled (non-exiting) */
        while (!(notify_enabled_ppg || notify_enabled_accel || notify_enabled_temp)) {
            k_msleep(100);
        }
       

        /* Active collection loop: loop until TEMP triggers end-of-cycle */
        while (notify_enabled_ppg || notify_enabled_accel || notify_enabled_temp) {
            uint64_t now = k_uptime_get();

            /* PPG pacing: one read every ~10 ms (adjust if needed) */
            if (notify_enabled_ppg && (p_cnt < MAX_SAMPLES) && (now - last_ppg >= 10)) {
                last_ppg = now;
                if (k_mutex_lock(&i2c_bus_mutex, K_MSEC(3)) == 0) {
                    if (adpd144ri_read_2ch(i2c_dev, ppg_sample.channels) == 0) {
                        ppg_sample.timestamp_ms = get_timestamp_ms();
                        if (k_msgq_put(&ppg_msgq, &ppg_sample, K_NO_WAIT) != 0) {
                            ppg_drops++;
                            if ((ppg_drops & 0x3F) == 0) {
                                printk("[SENSOR] PPG drops=%u\n", ppg_drops);
                            }
                        } else {
                            p_cnt++;
                        }
                    }
                    k_mutex_unlock(&i2c_bus_mutex);
                }
            }

            k_msleep(7);

            /* ACCEL pacing: one read every ~20 ms (adjust if needed) */
            if (notify_enabled_accel && (a_cnt < MAX_SAMPLES) && (now - last_accel >= 10)) {
                last_accel = now;
                if (k_mutex_lock(&i2c_bus_mutex, K_MSEC(3)) == 0) {
                    if (lis3dh_read_data(&lis3dh_dev, accel_sample.data)) {
                        accel_sample.timestamp_ms = get_timestamp_ms();
                        if (k_msgq_put(&accel_msgq, &accel_sample, K_NO_WAIT) != 0) {
                            accel_drops++;
                            if ((accel_drops & 0x3F) == 0) {
                                printk("[SENSOR] ACCEL drops=%u\n", accel_drops);
                            }
                        } else {
                            a_cnt++;
                        }
                    }
                    k_mutex_unlock(&i2c_bus_mutex);
                }
            }

            k_msleep(7);

            /* TEMP end-of-cycle trigger (unchanged) */
            if (notify_enabled_temp && p_cnt >= MAX_SAMPLES && a_cnt >= MAX_SAMPLES) {
                k_msleep(10);
                if (temp_sensor_read(&temp_sample) == 0) {
                    temp_sample.timestamp_ms = get_timestamp_ms();
                    if (k_msgq_put(&temp_msgq, &temp_sample, K_MSEC(3)) != 0) {
                        printk("[SENSOR] TEMP msgq put failed\n");
                    }
                } else {
                    printk("[SENSOR] TEMP read failed\n");
                }
                printk("[SENSOR] Collected FULL batch: PPG=%u, ACCEL=%u\n", p_cnt, a_cnt);
                break; /* let formatter handle sending/reset */
            }

            /* light backoff so other threads (formatter, BLE) run */
            k_msleep(40);
        } /* inner active loop */
    } /* outer forever */
}

void printer_thread(void *p1, void *p2, void *p3)
{
    printk("[THREAD] printer_thread started\n");

    PPGSample ppg_batch[BATCH_SIZE];
    lis3dh_data_t accel_batch[BATCH_SIZE];
    TempSample temp_sample;
    char ts[64];

    int total_ppg_sent = 0;
    int total_accel_sent = 0;
    bool temp_sent = false;

    bool p_flag=false;
    bool a_flag=false;

    while (1) {
        /* wait until any notify enabled */
        while (!(notify_enabled_ppg || notify_enabled_accel || notify_enabled_temp)) {
            k_msleep(50);
        }

        int ppg_count = 0, accel_count = 0, temp_count = 0;

        /* collect exactly BATCH_SIZE items for each stream if available */
        if (notify_enabled_ppg && k_msgq_num_used_get(&ppg_msgq) >= BATCH_SIZE) {
            for (int i = 0; i < BATCH_SIZE; i++) {
                if (k_msgq_get(&ppg_msgq, &ppg_batch[i], K_NO_WAIT) != 0) {
                    ppg_count = i;
                    break;
                }
            }
            if (ppg_count == 0) ppg_count = BATCH_SIZE;
            total_ppg_sent += ppg_count;
        }

        if (notify_enabled_accel && k_msgq_num_used_get(&accel_msgq) >= BATCH_SIZE) {
            for (int i = 0; i < BATCH_SIZE; i++) {
                if (k_msgq_get(&accel_msgq, &accel_batch[i], K_NO_WAIT) != 0) {
                    accel_count = i;
                    break;
                }
            }
            if (accel_count == 0) accel_count = BATCH_SIZE;
            total_accel_sent += accel_count;
        }

        /* TEMP: only when both streams have reached MAX_SAMPLES */
        if (notify_enabled_temp && !temp_sent &&
            total_ppg_sent >= MAX_SAMPLES && total_accel_sent >= MAX_SAMPLES &&
            k_msgq_num_used_get(&temp_msgq) >= 1) {
            k_msleep(20);
            if (k_msgq_get(&temp_msgq, &temp_sample, K_NO_WAIT) == 0) {
                temp_count = 1;
                temp_sent = true;
            }
        }

        /* nothing to do: small sleep */
        if (ppg_count == 0 && accel_count == 0 && temp_count == 0) {
            k_msleep(2);
            continue;
        } 

        /* build formatted strings (all heavy ops here) */
        k_mutex_lock(&notify_buf_mutex, K_FOREVER);

        if (ppg_count > 0) {
            int pos = 0, len = SENSOR_NOTIFY_BUF_SIZE;
            if(!p_flag){
                 pos += snprintf(sensor_notify_buf_ppg + pos, len - pos,
                    "PPG_BATCH:%d", patient_id);
            }
            for (int i = 0; i < ppg_count; i++) {
             //   format_timestamp(ppg_batch[i].timestamp_ms, ts, sizeof(ts));
               // uint64_t ts_ms = parse_timestamp_ms(ts);
                pos += snprintk(sensor_notify_buf_ppg + pos, len - pos,
                                "#%llu,%lx,%lx",
                                (unsigned long long)ppg_batch[i].timestamp_ms,
                                (unsigned long)ppg_batch[i].channels[0],
                                (unsigned long)ppg_batch[i].channels[1]);
                    
                    p_flag=true;
                if (pos >= len - 64) break;
            }
            sensor_notify_buf_ppg[len - 1] = '\0';
        }

        if (accel_count > 0) {
            int pos = 0, len = SENSOR_NOTIFY_BUF_SIZE;
             if(!a_flag){
                    pos += snprintf(sensor_notify_buf_accel + pos, len - pos, "ACCEL_BATCH:%d",patient_id);
            }
            for (int i = 0; i < accel_count; i++) {
            //    format_timestamp(accel_batch[i].timestamp_ms, ts, sizeof(ts));
              //  uint64_t ts_ms = parse_timestamp_ms(ts);
                pos += snprintk(sensor_notify_buf_accel + pos, len - pos,
                                "#%llu,%d,%d,%d",
                                (unsigned long long)accel_batch[i].timestamp_ms,
                                accel_batch[i].data[0],
                                accel_batch[i].data[1],
                                accel_batch[i].data[2]);

                        a_flag=true;
                if (pos >= len - 64) break;
            }
            sensor_notify_buf_accel[len - 1] = '\0';
        }

        if (temp_count > 0) {
            int pos = 0, len = SENSOR_NOTIFY_BUF_SIZE;
            pos += snprintf(sensor_notify_buf_temp + pos, len - pos, "TEMP_BATCH:");
          //  format_timestamp(temp_sample.timestamp_ms, ts, sizeof(ts));
          //  uint64_t ts_ms = parse_timestamp_ms(ts);
            pos += snprintk(sensor_notify_buf_temp + pos, len - pos,
                            "%d#%llu,%d,%d",patient_id,
                            (unsigned long long)temp_sample.timestamp_ms,
                            temp_sample.temperature_c, (int)temp_sample.battery_pct);
            sensor_notify_buf_temp[len - 1] = '\0';
        }

        k_mutex_unlock(&notify_buf_mutex);

        if (current_conn) {
            if (ppg_count > 0) bt_gatt_notify(current_conn, ppg_char_attr, sensor_notify_buf_ppg, strlen(sensor_notify_buf_ppg));
            if (accel_count > 0) bt_gatt_notify(current_conn, accel_char_attr, sensor_notify_buf_accel, strlen(sensor_notify_buf_accel));
            if (temp_count > 0) bt_gatt_notify(current_conn, temp_char_attr, sensor_notify_buf_temp, strlen(sensor_notify_buf_temp));
        }

        /* full-cycle handling: reset counters + safe RTC sleep */
        if (temp_sent) {
            printk("[THREAD] Full data cycle sent. Preparing sleep...\n");
            send_ack_to_mobile("1#0");

            total_ppg_sent = 0;
            total_accel_sent = 0;
            temp_sent = false;

            p_cnt = 0;
            a_cnt = 0;

            p_flag = false;
            a_flag = false;
          

            rtc2_set_alarm(50000); // 50s

            /* Safe non-blocking wait: check alarm flag periodically,
               use SEV/WFE to avoid missed wakeups. */
            while (!rtc2_alarm_triggered()) {
                __SEV();  /* ensure there is a pending event to wake WFE */
                __WFE();
                /* still loop checking alarm flag; this keeps other threads runnable */
            }
            printk("[THREAD] Woke up, starting new cycle.\n");
            send_ack_to_mobile("2#0");
        }
    } /* while */
}


#define INIT_THREAD_STACK 512
#define INIT_THREAD_PRIO   5  // Lower than sensor_thread (10)

// Thread function for LED/Button/GPIO init
void init_thread(void *p1, void *p2, void *p3)
{
    printk("[INIT] Initialization thread started\n");

    // GPIO high
    gpio_high();

    // WS2812 init
    ws2812_init();
    k_msleep(15);
    ws2812_set_color(&GREEN);

    // Button init
    if (button_init() != 0) {
        printk("[INIT] Button init failed\n");
    }

    printk("[INIT] LED, Button, GPIO init done\n");

    // Thread can now terminate
}

// Create the thread
//K_THREAD_DEFINE(init_tid, INIT_THREAD_STACK, init_thread, NULL, NULL, NULL, INIT_THREAD_PRIO, 0, 0);



int main(void)
{
    printk("\n********* BOOTING ADPD144RI BLE SENSOR DEMO *********\n");  

    k_mutex_init(&notify_buf_mutex);

    memset(sensor_notify_buf_ppg, 0, sizeof(sensor_notify_buf_ppg));
    memset(sensor_notify_buf_temp, 0, sizeof(sensor_notify_buf_temp));
    memset(sensor_notify_buf_accel, 0, sizeof(sensor_notify_buf_accel));

    int err = bt_enable(bt_ready);
    printk("[MAIN] Called bt_enable(), returned %d\n", err);
    if (err) {
        printk("[BLE] Bluetooth init failed (err %d)\n", err);
    }

    k_work_init(&ble_disconnect_work, ble_disconnect_work_handler);

    /* --- Create threads dynamically --- */
    k_thread_create(&sensor_tid_data, sensor_stack, SENSOR_STACK_SIZE,
                sensor_thread, NULL, NULL, NULL,
                SENSOR_PRIO, 0, K_NO_WAIT);

    k_thread_create(&printer_tid_data, printer_stack, PRINTER_STACK_SIZE,
                printer_thread, NULL, NULL, NULL,
                PRINTER_PRIO, 0, K_NO_WAIT);
    

    k_thread_create(&init_tid_data, init_stack, INIT_STACK_SIZE,
                    init_thread, NULL, NULL, NULL,
                    INIT_PRIO, 0, K_NO_WAIT);

    while (1) {
        k_msleep(1000);
    }

    return 0;
}