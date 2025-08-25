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

#define I2C_NODE DT_NODELABEL(i2c0)

#define BATCH_SIZE 6
#define MSGQ_SIZE 20

#define MAX_SAMPLES 1998

#define I2C_MUTEX_TIMEOUT  K_MSEC(5)  // Max wait time for I2C access

static uint16_t a_cnt=0, p_cnt=0;

K_MUTEX_DEFINE(i2c_bus_mutex);

K_MSGQ_DEFINE(ppg_msgq, sizeof(PPGSample), MSGQ_SIZE, 4);
K_MSGQ_DEFINE(temp_msgq, sizeof(TempSample), MSGQ_SIZE, 4);
K_MSGQ_DEFINE(accel_msgq, sizeof(lis3dh_data_t), MSGQ_SIZE, 4);

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

void sensor_thread(void)
{
    const struct device *i2c_dev = DEVICE_DT_GET(I2C_NODE);

    printk("[THREAD] sensor_thread started\n");

    if (!device_is_ready(i2c_dev)) {
        printk("[I2C] device %s not ready!\n", i2c_dev->name);
        return;
    }

    // Init LIS3DH
    if (k_mutex_lock(&i2c_bus_mutex, I2C_MUTEX_TIMEOUT) == 0) {
        if (!lis3dh_init(&lis3dh_dev, i2c_dev, 0x18)) {
            printk("[LIS3DH] Accelerometer sensor initialization failed\n");
        }
        k_mutex_unlock(&i2c_bus_mutex);
    }

    // Init PPG
    if (k_mutex_lock(&i2c_bus_mutex, I2C_MUTEX_TIMEOUT) == 0) {
        if (adpd144ri_configure_spo2_hr(i2c_dev) != 0) {
            printk("[PPG] PPG Sensor failed\n");
        }
        k_mutex_unlock(&i2c_bus_mutex);
    }

    // Re-init LIS3DH after PPG setup
    if (k_mutex_lock(&i2c_bus_mutex, I2C_MUTEX_TIMEOUT) == 0) {
        if (!lis3dh_init(&lis3dh_dev, i2c_dev, 0x18)) {
            printk("[LIS3DH] Accelerometer sensor re-init failed\n");
        }
        k_mutex_unlock(&i2c_bus_mutex);
    }

    // Temp sensor init
    if (temp_sensor_init() != 0) {
        printk("[TEMP] Temperature sensor initialization failed\n");
    }

    rtc2_init();

    PPGSample ppg_sample;
    lis3dh_data_t accel_sample;
    TempSample temp_sample;

    while (1) {
        while (!(notify_enabled_ppg || notify_enabled_temp || notify_enabled_accel)) {
            k_msleep(200);
        }

        while (notify_enabled_ppg || notify_enabled_accel || notify_enabled_temp) {

            // --- Read PPG ---
            if (p_cnt < MAX_SAMPLES && notify_enabled_ppg) {
                if (k_mutex_lock(&i2c_bus_mutex, I2C_MUTEX_TIMEOUT) == 0) {
                    if (adpd144ri_read_2ch(i2c_dev, ppg_sample.channels) == 0) {
                        ppg_sample.timestamp_ms = get_timestamp_ms();
                        k_msgq_put(&ppg_msgq, &ppg_sample, K_NO_WAIT);
                        p_cnt++;
                    }
                    k_mutex_unlock(&i2c_bus_mutex);
                }
            }

            // --- Read ACCEL ---
            if (a_cnt < MAX_SAMPLES && notify_enabled_accel) {
                if (k_mutex_lock(&i2c_bus_mutex, I2C_MUTEX_TIMEOUT) == 0) {
                    if (lis3dh_read_data(&lis3dh_dev, accel_sample.data)) {
                        accel_sample.timestamp_ms = get_timestamp_ms();
                        k_msgq_put(&accel_msgq, &accel_sample, K_NO_WAIT);
                        a_cnt++;
                    }
                    k_mutex_unlock(&i2c_bus_mutex);
                }
            }

            // --- Read TEMP ---
            if (p_cnt >= MAX_SAMPLES && a_cnt >= MAX_SAMPLES && notify_enabled_temp) {
                k_msleep(100);
                if (temp_sensor_read(&temp_sample) == 0) {
                    temp_sample.timestamp_ms = get_timestamp_ms();
                    k_msgq_put(&temp_msgq, &temp_sample, K_NO_WAIT);
                }
                printk("[SENSOR] Collected FULL batch: PPG=%d, ACCEL=%d, TEMP=1\n", p_cnt, a_cnt);
                // Note: We do NOT reset counters or sleep here
                break;
            }

            k_msleep(30);
        }
    }
}

#define WAIT_MS 10  // how long to wait before checking again

void printer_thread(void)
{
    printk("[THREAD] printer_thread started\n");

    PPGSample ppg_batch[BATCH_SIZE];
    TempSample temp_sample;
    lis3dh_data_t accel_batch[BATCH_SIZE];
    char ts[64];
    int total_ppg_sent = 0;
    int total_accel_sent = 0;
    bool temp_sent = false;

    while (1) {
        int ppg_count = 0, temp_count = 0, accel_count = 0;

        // --- Collect PPG batch ---
        if (notify_enabled_ppg &&
            k_msgq_num_used_get(&ppg_msgq) >= BATCH_SIZE) 
        {
            for (int i = 0; i < BATCH_SIZE; i++) {
                k_msgq_get(&ppg_msgq, &ppg_batch[i], K_NO_WAIT);
            }
            ppg_count = BATCH_SIZE;
            total_ppg_sent += ppg_count;
        }

        // --- Collect ACCEL batch ---
        if (notify_enabled_accel &&
            k_msgq_num_used_get(&accel_msgq) >= BATCH_SIZE) 
        {
            for (int i = 0; i < BATCH_SIZE; i++) {
                k_msgq_get(&accel_msgq, &accel_batch[i], K_NO_WAIT);
            }
            accel_count = BATCH_SIZE;
            total_accel_sent += accel_count;
        }

        // --- Collect TEMP after MAX_SAMPLES ---
        if (notify_enabled_temp &&
            !temp_sent &&
            total_ppg_sent >= MAX_SAMPLES &&
            total_accel_sent >= MAX_SAMPLES &&
            k_msgq_num_used_get(&temp_msgq) >= 1) 
        {
            k_msleep(100);
            k_msgq_get(&temp_msgq, &temp_sample, K_NO_WAIT);
            temp_count = 1;
            temp_sent = true;
        }

        // If no batches ready, wait a bit
        if (ppg_count == 0 && temp_count == 0 && accel_count == 0) {
            k_msleep(WAIT_MS);
            continue;
        }

        // Lock buffer before writing
        k_mutex_lock(&notify_buf_mutex, K_FOREVER);

        // --- Format and prepare BLE data ---
        if (ppg_count > 0) {
            int pos = 0, len = SENSOR_NOTIFY_BUF_SIZE;
            pos += snprintf(sensor_notify_buf_ppg + pos, len - pos, "PPG_BATCH:");
            for (int i = 0; i < ppg_count; i++) {
                format_timestamp(ppg_batch[i].timestamp_ms, ts, sizeof(ts));
                uint64_t ts_ms = parse_timestamp_ms(ts);
                pos += snprintk(sensor_notify_buf_ppg + pos, len - pos,
                                "#%llu,%lx,%lx",
                                (unsigned long long)ts_ms,
                                (unsigned long)ppg_batch[i].channels[0],
                                (unsigned long)ppg_batch[i].channels[1]);
            }
            sensor_notify_buf_ppg[len - 1] = '\0';
        }

        if (accel_count > 0) {
            int pos = 0, len = SENSOR_NOTIFY_BUF_SIZE;
            pos += snprintf(sensor_notify_buf_accel + pos, len - pos, "ACCEL_BATCH:");
            for (int i = 0; i < accel_count; i++) {
                format_timestamp(accel_batch[i].timestamp_ms, ts, sizeof(ts));
                uint64_t ts_ms = parse_timestamp_ms(ts);
                pos += snprintk(sensor_notify_buf_accel + pos, len - pos,
                                "#%llu,%d,%d,%d",
                                (unsigned long long)ts_ms,
                                accel_batch[i].data[0],
                                accel_batch[i].data[1],
                                accel_batch[i].data[2]);
            }
            sensor_notify_buf_accel[len - 1] = '\0';
        }

        if (temp_count > 0) {
            int pos = 0, len = SENSOR_NOTIFY_BUF_SIZE;
            pos += snprintf(sensor_notify_buf_temp + pos, len - pos, "TEMP_BATCH:");
            format_timestamp(temp_sample.timestamp_ms, ts, sizeof(ts));
            uint64_t ts_ms = parse_timestamp_ms(ts);
            pos += snprintk(sensor_notify_buf_temp + pos, len - pos,
                            "#%llu,%d,%d",
                            (unsigned long long)ts_ms,
                            temp_sample.temperature_c, (int)temp_sample.battery_pct);
            sensor_notify_buf_temp[len - 1] = '\0';
        }

        k_mutex_unlock(&notify_buf_mutex);

         if (ppg_count > 0)
            printk("[PRINTER] Notify Data (PPG):\n%s\n", sensor_notify_buf_ppg);
        if (temp_count > 0)
            printk("[PRINTER] Notify Data (TEMP):\n%s\n", sensor_notify_buf_temp);
        if (accel_count > 0)
            printk("[PRINTER] Notify Data (ACCEL):\n%s\n", sensor_notify_buf_accel);

        // --- Send via BLE ---
        if (current_conn) {
            if (ppg_count > 0)
                bt_gatt_notify(current_conn, ppg_char_attr, sensor_notify_buf_ppg, strlen(sensor_notify_buf_ppg));
            if (temp_count > 0)
                bt_gatt_notify(current_conn, temp_char_attr, sensor_notify_buf_temp, strlen(sensor_notify_buf_temp));
            if (accel_count > 0)
                bt_gatt_notify(current_conn, accel_char_attr, sensor_notify_buf_accel, strlen(sensor_notify_buf_accel));
        }

        // --- After sending TEMP (final batch of this cycle) ---
        if (temp_sent) {
            printk("[THREAD] All data sent. Going to System ON sleep...\n");

            // Reset counters for next cycle
            total_ppg_sent = 0;
            total_accel_sent = 0;
            temp_sent = false;

            // Reset sensor thread counters so collection restarts
            p_cnt = 0;
            a_cnt = 0;


            // Set alarm for 50 seconds from now
            rtc2_set_alarm(50000);

            // Sleep until alarm triggers
            while (!rtc2_alarm_triggered()) {
                __WFE(); // Wait for event
            }

            printk("[THREAD] Woke up, starting new data collection cycle.\n");
        }
    }
}



K_THREAD_DEFINE(sensor_tid, 1280, sensor_thread, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(printer_tid, 2560, printer_thread, NULL, NULL, NULL, 5, 0, 0);




int main(void)
{
    printk("\n********* BOOTING ADPD144RI BLE SENSOR DEMO *********\n");  
   
    
    ws2812_init();
    k_msleep(50); 
    ws2812_set_color(&GREEN);

    if (button_init() != 0) {
        return;
    }

    k_mutex_init(&notify_buf_mutex);

    memset(sensor_notify_buf_ppg, 0, sizeof(sensor_notify_buf_ppg));
    memset(sensor_notify_buf_temp, 0, sizeof(sensor_notify_buf_temp));
    memset(sensor_notify_buf_accel, 0, sizeof(sensor_notify_buf_accel));

    int err = bt_enable(bt_ready);
    printk("[MAIN] Called bt_enable(), returned %d\n", err);
    if (err) {
        printk("[BLE] Bluetooth init failed (err %d)\n", err);
    }
      /* Keep main alive forever */
    for (;;) {
        k_sleep(K_FOREVER);
    }
     return 0;
}
