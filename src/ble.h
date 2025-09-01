#ifndef BLE_H_
#define BLE_H_

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <stdbool.h>

#define SENSOR_NOTIFY_BUF_SIZE 4096

#include <zephyr/bluetooth/bluetooth.h>

#define BLE_TIMEOUT_MS 6000 //180000  // 3 minutes

static struct k_timer ble_disconnect_timer;

// Notification buffers for each sensor
extern char sensor_notify_buf_ppg[SENSOR_NOTIFY_BUF_SIZE];
extern char sensor_notify_buf_temp[SENSOR_NOTIFY_BUF_SIZE];
extern char sensor_notify_buf_accel[SENSOR_NOTIFY_BUF_SIZE];

// GATT service static struct
extern const struct bt_gatt_service_static ppg_svc;
extern const struct bt_gatt_service_static temp_svc;
extern const struct bt_gatt_service_static accel_svc;

// Attribute pointers to characteristic values for notifications
extern const struct bt_gatt_attr *ppg_char_attr;
extern const struct bt_gatt_attr *temp_char_attr;
extern const struct bt_gatt_attr *accel_char_attr;

// BLE connection object
extern struct bt_conn *current_conn;

// Per-characteristic notification enabled flags
extern volatile bool notify_enabled_ppg;
extern volatile bool notify_enabled_temp;
extern volatile bool notify_enabled_accel;

// Mutex protecting notification buffers
extern struct k_mutex notify_buf_mutex;

// BLE function callbacks
void bt_ready(int err);
void connected(struct bt_conn *conn, uint8_t err);
void disconnected(struct bt_conn *conn, uint8_t reason);
//void ble_disconnect_timeout(struct k_timer *dummy);

#endif 
