#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* NimBLE includes */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"

static const char *TAG = "BLE";

/* ---------- Shared Data ---------- */
struct health_struc {
    uint8_t heart_rate;
    uint8_t o2;
    uint8_t battery;
    uint8_t padding;
    float body_temp;
};
struct health_struc received_data;
bool ble_data_ready = false;

/* ---------- Connection & Characteristic Handle ---------- */
static uint16_t current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t current_data_val_handle = 0;

/* ---------- UUID Definitions ---------- */
static const ble_uuid128_t system_uuid = BLE_UUID128_INIT(
    0x1b,0x02,0x3b,0xcc,0xb3,0xbf,0x72,0xb0,
    0xdc,0x46,0x6b,0x79,0x3a,0x34,0x1c,0xdb
);

static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x1b,0x02,0x3b,0xcc,0xb3,0xbf,0x72,0xb0,
    0xdc,0x46,0x6b,0x79,0x3a,0x34,0x1c,0xdb
);

static const ble_uuid128_t current_data_uuid = BLE_UUID128_INIT(
    0x17,0xd1,0xe6,0xca,0x64,0xbd,0x5b,0xba,
    0xce,0x4d,0xcc,0x33,0x54,0x49,0x69,0x43
);

/* ---------- Forward Declarations ---------- */
static void ble_host_task(void *param);
static int char_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg);
static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg);
static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void scan_task(void *arg);
static void reset_cb(int reason);

/* ---------- NimBLE Host Task ---------- */
static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------- Read callback — called when server responds to our read ---------- */
static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(TAG, "Read error; status=%d", error->status);
        return 0;
    }

    int len = OS_MBUF_PKTLEN(attr->om);
    if (len >= sizeof(struct health_struc)) {
        os_mbuf_copydata(attr->om, 0, sizeof(struct health_struc), &received_data);
        ESP_LOGI(TAG, "HR:%d O2:%d Batt:%d Temp:%.1f",
                 received_data.heart_rate,
                 received_data.o2,
                 received_data.battery,
                 received_data.body_temp);
        ble_data_ready = true;
    } else {
        ESP_LOGW(TAG, "Read response too short (%d bytes)", len);
    }

    return 0;
}

/* ---------- Poll task — reads characteristic every 2 seconds ---------- */
static void poll_task(void *arg)
{
    while (1) {
        if (current_conn_handle != BLE_HS_CONN_HANDLE_NONE && current_data_val_handle != 0) {
            int rc = ble_gattc_read(current_conn_handle, current_data_val_handle, read_cb, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Read failed; rc=%d", rc);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); // poll every 2 seconds
    }
}

/* ---------- Characteristic discovery callback ---------- */
static int char_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status != 0) return 0;

    if (ble_uuid_cmp(&chr->uuid.u, &current_data_uuid.u) == 0) {
        ESP_LOGI(TAG, "Found target characteristic (handle=%d)", chr->val_handle);
        // Save the handle so poll_task can use it
        current_data_val_handle = chr->val_handle;
    }
    return 0;
}

/* ---------- Service discovery callback ---------- */
static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status != 0) return 0;

    if (ble_uuid_cmp(&svc->uuid.u, &service_uuid.u) == 0) {
        ESP_LOGI(TAG, "Found target service (start=%d end=%d)", svc->start_handle, svc->end_handle);
        ble_gattc_disc_all_chrs(conn_handle, svc->start_handle, svc->end_handle, char_disc_cb, NULL);
    }
    return 0;
}

/* ---------- GAP event handler ---------- */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0) {
                for (int i = 0; i < fields.num_uuids128; i++) {
                    if (ble_uuid_cmp(&fields.uuids128[i].u, &system_uuid.u) == 0) {
                        ESP_LOGI(TAG, "Found device, connecting...");
                        ble_gap_disc_cancel();
                        uint8_t own_addr_type;
                        ble_hs_id_infer_auto(0, &own_addr_type);
                        ble_gap_connect(own_addr_type, &event->disc.addr, 30000, NULL,
                                        gap_event_cb, NULL);
                        return 0;
                    }
                }
            }
            break;
        }

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Connected, discovering services...");
                current_conn_handle = event->connect.conn_handle;
                ble_gattc_disc_all_svcs(event->connect.conn_handle, svc_disc_cb, NULL);
            } else {
                ESP_LOGE(TAG, "Connection failed; status=%d", event->connect.status);
                current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
            current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            current_data_val_handle = 0;
            break;

        default:
            break;
    }
    return 0;
}

/* ---------- BLE scan task ---------- */
static void scan_task(void *arg)
{
    struct ble_gap_disc_params disc_params = {0};
    uint8_t own_addr_type;
    ble_hs_id_infer_auto(0, &own_addr_type);

    disc_params.passive = 1;
    disc_params.filter_duplicates = 1;

    while (1) {
        // Only scan if not connected
        if (current_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "Scanning for BLE devices...");
            ble_gap_disc(own_addr_type, 3000, &disc_params, gap_event_cb, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

/* ---------- BLE sync callback ---------- */
void ble_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    // Start scan task
    xTaskCreatePinnedToCore(scan_task, "ble_scan", 8192, NULL, 3, NULL, 1);
    // Start poll task
    xTaskCreatePinnedToCore(poll_task, "ble_poll", 4096, NULL, 2, NULL, 1);
}

/* ---------- BLE reset callback ---------- */
static void reset_cb(int reason)
{
    ESP_LOGE(TAG, "BLE reset; reason=%d", reason);
}

/* ---------- BLE init ---------- */
void ble_init(void)
{
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = reset_cb;
    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.store_status_cb = NULL;

    ble_svc_gap_device_name_set("Little Guardian");

    nimble_port_freertos_init(ble_host_task);
}