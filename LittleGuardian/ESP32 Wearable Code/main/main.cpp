/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stdint.h>
#include <stdio.h>
#include <cstring>

#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"  // ESP32 internal temperature sensor
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* BLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "bleprph.h"

#include "heartRate.h"
#include "spo2_algorithm.h"

// ============================================================
//  BLE configuration
// ============================================================

static const char *tag         = "NimBLE_BLE_PRPH";
static const char *device_name = "Little Guardian";
static uint8_t    own_addr_type;

static int bleprph_gap_event(struct ble_gap_event *event, void *arg);

// System UUID: db1c343a-796b-46dc-b072-bfb3cc3b021b
static const ble_uuid128_t system_uuid =
    BLE_UUID128_INIT(0x1b, 0x02, 0x3b, 0xcc, 0xb3, 0xbf, 0x72, 0xb0,
                     0xdc, 0x46, 0x6b, 0x79, 0x3a, 0x34, 0x1c, 0xdb);

// ============================================================
//  MAX30102 / health-monitor configuration
// ============================================================

#define I2C_PORT              I2C_NUM_0
#define I2C_SDA_PIN           5
#define I2C_SCL_PIN           6
#define I2C_CLOCK_HZ          400000
#define MAX30102_ADDR         0x57

#define SAMPLE_COUNT          BUFFER_SIZE
#define SAMPLE_TIMEOUT_US     (15ULL * 1000ULL * 1000ULL)

// --- CHANGED: match the browser's 10-second poll interval ---
#define MEASUREMENT_PERIOD_US (10ULL * 1000ULL * 1000ULL)

#define MIN_SLEEP_US          (1ULL  * 1000ULL * 1000ULL)
#define HEART_RATE_AVG_COUNT  4
#define I2C_TIMEOUT_MS        1000

// --- REMOVED: TEST_ENV_CONTINUOUS and TEST_ENV_INTERVAL_MS ---
//     Light sleep loop replaces both modes.

#define NIMBLE_HOST_STACK_SIZE 8192  // Increased from default 4096

static const char *TAG = "health_monitor";

// ============================================================
//  Internal temperature sensor configuration
// ============================================================

#define INTERNAL_TEMP_THRESHOLD_C   42.0f   // Deep sleep if chip exceeds this
#define INTERNAL_TEMP_POLL_MS       10000   // How often to check chip temp

static temperature_sensor_handle_t s_temp_sensor        = NULL;
static temperature_sensor_config_t s_temp_sensor_config =
    TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);

// ============================================================
//  MAX30102 register map
// ============================================================

static constexpr uint8_t REG_INTSTAT2       = 0x01;
static constexpr uint8_t REG_INTENABLE2     = 0x03;
static constexpr uint8_t REG_FIFO_WRITE_PTR = 0x04;
static constexpr uint8_t REG_FIFO_OVERFLOW  = 0x05;
static constexpr uint8_t REG_FIFO_READ_PTR  = 0x06;
static constexpr uint8_t REG_FIFO_DATA      = 0x07;
static constexpr uint8_t REG_FIFO_CONFIG    = 0x08;
static constexpr uint8_t REG_MODE_CONFIG    = 0x09;
static constexpr uint8_t REG_SPO2_CONFIG    = 0x0A;
static constexpr uint8_t REG_LED1_PA        = 0x0C;
static constexpr uint8_t REG_LED2_PA        = 0x0D;
static constexpr uint8_t REG_MULTI_LED1     = 0x11;
static constexpr uint8_t REG_MULTI_LED2     = 0x12;
static constexpr uint8_t REG_TEMP_INT       = 0x1F;
static constexpr uint8_t REG_TEMP_FRAC      = 0x20;
static constexpr uint8_t REG_TEMP_CONFIG    = 0x21;
static constexpr uint8_t REG_PART_ID        = 0xFF;

static constexpr uint8_t PART_ID_VALUE      = 0x15;
static constexpr uint8_t MODE_RESET         = 0x40;
static constexpr uint8_t MODE_SHUTDOWN      = 0x80;
static constexpr uint8_t MODE_RED_IR        = 0x03;
static constexpr uint8_t SAMPLE_AVG_4       = 0x40;
static constexpr uint8_t FIFO_ROLLOVER_EN   = 0x10;
static constexpr uint8_t ADC_RANGE_4096     = 0x20;
static constexpr uint8_t SAMPLE_RATE_100    = 0x04;
static constexpr uint8_t PULSE_WIDTH_411    = 0x03;
static constexpr uint8_t TEMP_RDY_EN        = 0x02;
static constexpr uint8_t SLOT_RED_LED       = 0x01;
static constexpr uint8_t SLOT_IR_LED        = 0x02;

static uint32_t s_ir_buffer[SAMPLE_COUNT];
static uint32_t s_red_buffer[SAMPLE_COUNT];

typedef struct {
    float   temperature_c;
    int32_t heart_rate_bpm;
    bool    heart_rate_valid;
    int32_t spo2_percent;
    bool    spo2_valid;
} measurement_t;

// ============================================================
//  Internal temperature sensor — init & monitoring task
// ============================================================

static void initialize_internal_temp(void)
{
    ESP_LOGI(TAG, "Installing internal temperature sensor (range 20–100 °C)");
    ESP_ERROR_CHECK(temperature_sensor_install(&s_temp_sensor_config,
                                               &s_temp_sensor));
    ESP_LOGI(TAG, "Enabling internal temperature sensor");
    ESP_ERROR_CHECK(temperature_sensor_enable(s_temp_sensor));
}

/**
 * @brief FreeRTOS task: polls the ESP32 die temperature every
 *        INTERNAL_TEMP_POLL_MS milliseconds.  If it exceeds
 *        INTERNAL_TEMP_THRESHOLD_C the device enters deep sleep
 *        immediately (all wakeup sources disabled) to prevent damage.
 *
 *        NOTE: this task survives light sleep — it simply resumes
 *        after wakeup as normal, which is the correct behaviour.
 */
static void internal_temp_monitor_task(void * /*param*/)
{
    while (true) {
        float chip_temp = 0.0f;
        ESP_ERROR_CHECK(temperature_sensor_get_celsius(s_temp_sensor,
                                                       &chip_temp));
        ESP_LOGI(TAG, "Internal chip temperature: %.2f °C", chip_temp);

        if (chip_temp > INTERNAL_TEMP_THRESHOLD_C) {
            ESP_LOGW(TAG,
                     "Chip temperature %.2f °C exceeds threshold %.2f °C — "
                     "shutting down BLE and entering deep sleep",
                     chip_temp, INTERNAL_TEMP_THRESHOLD_C);
            ble_gap_adv_stop();
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
            esp_deep_sleep_start();
            // Never reached
        }

        vTaskDelay(pdMS_TO_TICKS(INTERNAL_TEMP_POLL_MS));
    }
}

// ============================================================
//  I2C scanner
// ============================================================

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus (SDA=GPIO%d, SCL=GPIO%d)...",
             I2C_SDA_PIN, I2C_SCL_PIN);
    bool found_any = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd,
                                             pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02x", addr);
            found_any = true;
        }
    }
    if (!found_any) {
        ESP_LOGW(TAG, "  No I2C devices found — check wiring!");
    }
}

// ============================================================
//  MAX30102 helpers
// ============================================================

static void print_addr(const uint8_t *addr)
{
    ESP_LOGI(tag, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static esp_err_t max30102_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_write_to_device(
        I2C_PORT, MAX30102_ADDR, payload, sizeof(payload),
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t max30102_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(
        I2C_PORT, MAX30102_ADDR, &reg, 1, value, 1,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t max30102_read_bytes(uint8_t reg, uint8_t *buffer,
                                     size_t length)
{
    return i2c_master_write_read_device(
        I2C_PORT, MAX30102_ADDR, &reg, 1, buffer, length,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static bool max30102_clear_fifo(void)
{
    return max30102_write_reg(REG_FIFO_WRITE_PTR, 0) == ESP_OK &&
           max30102_write_reg(REG_FIFO_OVERFLOW,  0) == ESP_OK &&
           max30102_write_reg(REG_FIFO_READ_PTR,  0) == ESP_OK;
}

static bool max30102_init(void)
{
    i2c_config_t i2c_cfg = {};
    i2c_cfg.mode             = I2C_MODE_MASTER;
    i2c_cfg.sda_io_num       = I2C_SDA_PIN;
    i2c_cfg.scl_io_num       = I2C_SCL_PIN;
    i2c_cfg.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    i2c_cfg.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    i2c_cfg.master.clk_speed = I2C_CLOCK_HZ;
    i2c_cfg.clk_flags        = 0;

    if (i2c_param_config(I2C_PORT, &i2c_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed");
        return false;
    }
    if (i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed");
        return false;
    }

    i2c_scan();

    uint8_t part_id = 0;
    if (max30102_read_reg(REG_PART_ID, &part_id) != ESP_OK ||
        part_id != PART_ID_VALUE) {
        ESP_LOGE(TAG, "MAX30102 part ID check failed (got 0x%02x, expected 0x%02x)",
                 part_id, PART_ID_VALUE);
        return false;
    }
    ESP_LOGI(TAG, "MAX30102 part ID OK (0x%02x)", part_id);

    if (max30102_write_reg(REG_MODE_CONFIG, MODE_RESET) != ESP_OK) {
        ESP_LOGE(TAG, "MAX30102 reset failed");
        return false;
    }

    const int64_t reset_deadline_us = esp_timer_get_time() + 100000;
    while (esp_timer_get_time() < reset_deadline_us) {
        uint8_t mode_cfg = 0;
        if (max30102_read_reg(REG_MODE_CONFIG, &mode_cfg) != ESP_OK) {
            return false;
        }
        if ((mode_cfg & MODE_RESET) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (max30102_write_reg(REG_FIFO_CONFIG,
                           SAMPLE_AVG_4 | FIFO_ROLLOVER_EN)          != ESP_OK ||
        max30102_write_reg(REG_MODE_CONFIG, MODE_RED_IR)              != ESP_OK ||
        max30102_write_reg(REG_SPO2_CONFIG,
                           ADC_RANGE_4096 | SAMPLE_RATE_100 |
                           PULSE_WIDTH_411)                            != ESP_OK ||
        max30102_write_reg(REG_LED1_PA,    0x1F)                      != ESP_OK ||
        max30102_write_reg(REG_LED2_PA,    0x1F)                      != ESP_OK ||
        max30102_write_reg(REG_MULTI_LED1, SLOT_RED_LED |
                                           (SLOT_IR_LED << 4))        != ESP_OK ||
        max30102_write_reg(REG_MULTI_LED2, 0x00)                      != ESP_OK ||
        max30102_write_reg(REG_INTENABLE2, TEMP_RDY_EN)               != ESP_OK ||
        !max30102_clear_fifo()) {
        ESP_LOGE(TAG, "MAX30102 configuration failed");
        return false;
    }

    ESP_LOGI(TAG, "MAX30102 initialized successfully");
    return true;
}

static bool max30102_shutdown(void)
{
    uint8_t mode_cfg = 0;
    if (max30102_read_reg(REG_MODE_CONFIG, &mode_cfg) != ESP_OK) {
        return false;
    }
    return max30102_write_reg(REG_MODE_CONFIG,
                              static_cast<uint8_t>(mode_cfg | MODE_SHUTDOWN))
           == ESP_OK;
}

/**
 * @brief Wake the MAX30102 from shutdown by clearing the SHDN bit
 *        and restoring SpO2 mode, then flush the FIFO so stale
 *        samples from before sleep are discarded.
 */
static bool max30102_wakeup(void)
{
    uint8_t mode_cfg = 0;
    if (max30102_read_reg(REG_MODE_CONFIG, &mode_cfg) != ESP_OK) {
        return false;
    }
    // Clear SHDN bit, force SpO2 (Red+IR) mode
    mode_cfg = static_cast<uint8_t>((mode_cfg & ~MODE_SHUTDOWN) | MODE_RED_IR);
    if (max30102_write_reg(REG_MODE_CONFIG, mode_cfg) != ESP_OK) {
        return false;
    }
    // Give the sensor a moment to stabilise its LEDs
    vTaskDelay(pdMS_TO_TICKS(5));
    return max30102_clear_fifo();
}

static bool max30102_read_fifo_samples(
    uint32_t *red_samples, uint32_t *ir_samples,
    size_t max_samples, size_t *samples_read)
{
    uint8_t read_ptr  = 0;
    uint8_t write_ptr = 0;
    *samples_read = 0;

    if (max30102_read_reg(REG_FIFO_READ_PTR,  &read_ptr)  != ESP_OK ||
        max30102_read_reg(REG_FIFO_WRITE_PTR, &write_ptr) != ESP_OK) {
        return false;
    }

    int available = static_cast<int>(write_ptr) - static_cast<int>(read_ptr);
    if (available < 0)  { available += 32; }
    if (available == 0) { return true; }

    const size_t to_read =
        static_cast<size_t>(available) < max_samples
            ? static_cast<size_t>(available) : max_samples;

    uint8_t fifo_bytes[32 * 6] = {0};
    const size_t byte_count = to_read * 6U;

    if (max30102_read_bytes(REG_FIFO_DATA, fifo_bytes, byte_count) != ESP_OK) {
        return false;
    }

    for (size_t i = 0; i < to_read; ++i) {
        const size_t offset = i * 6U;
        red_samples[i] = (static_cast<uint32_t>(fifo_bytes[offset])     << 16) |
                         (static_cast<uint32_t>(fifo_bytes[offset + 1]) <<  8) |
                          static_cast<uint32_t>(fifo_bytes[offset + 2]);
        ir_samples[i]  = (static_cast<uint32_t>(fifo_bytes[offset + 3]) << 16) |
                         (static_cast<uint32_t>(fifo_bytes[offset + 4]) <<  8) |
                          static_cast<uint32_t>(fifo_bytes[offset + 5]);
        red_samples[i] &= 0x3FFFFu;
        ir_samples[i]  &= 0x3FFFFu;
    }

    *samples_read = to_read;
    return true;
}

static bool max30102_read_temperature(float *temperature_c)
{
    if (max30102_write_reg(REG_TEMP_CONFIG, 0x01) != ESP_OK) {
        return false;
    }

    const int64_t deadline_us = esp_timer_get_time() + 100000;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t int_status_2 = 0;
        if (max30102_read_reg(REG_INTSTAT2, &int_status_2) != ESP_OK) {
            return false;
        }
        if ((int_status_2 & TEMP_RDY_EN) != 0) {
            uint8_t temp_int  = 0;
            uint8_t temp_frac = 0;
            if (max30102_read_reg(REG_TEMP_INT,  &temp_int)  != ESP_OK ||
                max30102_read_reg(REG_TEMP_FRAC, &temp_frac) != ESP_OK) {
                return false;
            }
            *temperature_c =
                static_cast<float>(static_cast<int8_t>(temp_int))
                + (static_cast<float>(temp_frac) * 0.0625f);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

// ============================================================
//  Measurement logic
// ============================================================

static void print_measurement(const measurement_t *m)
{
    if (m->heart_rate_valid && m->spo2_valid) {
        printf("HR=%ld BPM SpO2=%ld%% Temp=%.2fC\n",
               static_cast<long>(m->heart_rate_bpm),
               static_cast<long>(m->spo2_percent),
               m->temperature_c);
    } else if (m->heart_rate_valid) {
        printf("HR=%ld BPM Temp=%.2fC (SpO2 invalid)\n",
               static_cast<long>(m->heart_rate_bpm),
               m->temperature_c);
    } else if (m->spo2_valid) {
        printf("SpO2=%ld%% Temp=%.2fC (HR invalid)\n",
               static_cast<long>(m->spo2_percent),
               m->temperature_c);
    } else {
        printf("Temp=%.2fC (HR/SpO2 invalid)\n", m->temperature_c);
    }
}

static bool collect_measurement(measurement_t *measurement)
{
    uint8_t  recent_rates[HEART_RATE_AVG_COUNT] = {0};
    size_t   recent_rate_count = 0;
    size_t   recent_rate_index = 0;
    int64_t  last_beat_ms      = 0;
    size_t   samples_collected = 0;
    const int64_t deadline_us  = esp_timer_get_time() + SAMPLE_TIMEOUT_US;

    while (samples_collected < SAMPLE_COUNT) {
        size_t batch_count = 0;
        if (!max30102_read_fifo_samples(
                &s_red_buffer[samples_collected],
                &s_ir_buffer[samples_collected],
                SAMPLE_COUNT - samples_collected,
                &batch_count)) {
            ESP_LOGW(TAG, "FIFO read failed");
            return false;
        }

        if (batch_count == 0) {
            if (esp_timer_get_time() >= deadline_us) {
                ESP_LOGW(TAG, "Timed out waiting for sensor samples");
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        for (size_t i = 0; i < batch_count; ++i) {
            const uint32_t ir = s_ir_buffer[samples_collected + i];
            if (checkForBeat(static_cast<int32_t>(ir))) {
                const int64_t now_ms = esp_timer_get_time() / 1000LL;
                if (last_beat_ms != 0) {
                    const int64_t delta_ms = now_ms - last_beat_ms;
                    if (delta_ms > 0) {
                        const float bpm =
                            60000.0f / static_cast<float>(delta_ms);
                        if (bpm >= 30.0f && bpm <= 220.0f) {
                            recent_rates[recent_rate_index] =
                                static_cast<uint8_t>(bpm);
                            recent_rate_index =
                                (recent_rate_index + 1U) %
                                HEART_RATE_AVG_COUNT;
                            if (recent_rate_count < HEART_RATE_AVG_COUNT) {
                                ++recent_rate_count;
                            }
                        }
                    }
                }
                last_beat_ms = now_ms;
            }
        }
        samples_collected += batch_count;
    }

    if (!max30102_read_temperature(&measurement->temperature_c)) {
        measurement->temperature_c = -999.0f;
    }

    int32_t algo_spo2       = 0;
    int32_t algo_heart_rate = 0;
    int8_t  spo2_valid      = 0;
    int8_t  hr_valid        = 0;

    maxim_heart_rate_and_oxygen_saturation(
        s_ir_buffer, SAMPLE_COUNT, s_red_buffer,
        &algo_spo2, &spo2_valid,
        &algo_heart_rate, &hr_valid);

    measurement->spo2_percent = algo_spo2;
    measurement->spo2_valid   = (spo2_valid != 0);

    if (recent_rate_count > 0) {
        uint32_t rate_sum = 0;
        for (size_t i = 0; i < recent_rate_count; ++i) {
            rate_sum += recent_rates[i];
        }
        measurement->heart_rate_bpm =
            static_cast<int32_t>(rate_sum / recent_rate_count);
        measurement->heart_rate_valid = true;
    } else {
        measurement->heart_rate_bpm   = algo_heart_rate;
        measurement->heart_rate_valid = (hr_valid != 0);
    }

    return true;
}

// ============================================================
//  Light sleep helper
// ============================================================

/**
 * @brief Shut down the MAX30102 and put the ESP32 into light sleep
 *        for sleep_time_us microseconds.  Unlike deep sleep, light
 *        sleep preserves RAM, FreeRTOS task state, and — crucially —
 *        the NimBLE/BLE radio, so the GATT connection stays alive.
 *        Execution resumes at the instruction after esp_light_sleep_start().
 */
static void sensor_light_sleep(uint64_t sleep_time_us)
{
    // Power down the MAX30102 LEDs while we sleep
    max30102_shutdown();

    ESP_LOGI(TAG, "Light sleep for %llu ms — BLE stays alive",
             static_cast<unsigned long long>(sleep_time_us / 1000ULL));

    esp_sleep_enable_timer_wakeup(sleep_time_us);
    esp_light_sleep_start();
    // ---- resumes here after wakeup ----

    ESP_LOGI(TAG, "Woke from light sleep — restarting sensor");

    // Bring the MAX30102 back out of shutdown and flush stale FIFO data
    if (!max30102_wakeup()) {
        ESP_LOGW(TAG, "MAX30102 wakeup failed — will retry next cycle");
    }
}

// ============================================================
//  BLE helpers
// ============================================================

#if NIMBLE_BLE_CONNECT
static void bleprph_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    MODLOG_DFLT(INFO, "handle=%d our_ota_addr_type=%d our_ota_addr=",
                desc->conn_handle, desc->our_ota_addr.type);
    print_addr(desc->our_ota_addr.val);
    MODLOG_DFLT(INFO, " our_id_addr_type=%d our_id_addr=",
                desc->our_id_addr.type);
    print_addr(desc->our_id_addr.val);
    MODLOG_DFLT(INFO, " peer_ota_addr_type=%d peer_ota_addr=",
                desc->peer_ota_addr.type);
    print_addr(desc->peer_ota_addr.val);
    MODLOG_DFLT(INFO, " peer_id_addr_type=%d peer_id_addr=",
                desc->peer_id_addr.type);
    print_addr(desc->peer_id_addr.val);
    MODLOG_DFLT(INFO,
                " conn_itvl=%d conn_latency=%d supervision_timeout=%d "
                "encrypted=%d authenticated=%d bonded=%d\n",
                desc->conn_itvl, desc->conn_latency,
                desc->supervision_timeout,
                desc->sec_state.encrypted,
                desc->sec_state.authenticated,
                desc->sec_state.bonded);
}
#endif

static void bleprph_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields  fields;
    struct ble_hs_adv_fields  rsp_fields;
    const char *name;
    int rc;

    memset(&fields, 0, sizeof fields);
    fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128             = &system_uuid;
    fields.num_uuids128         = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
        return;
    }

    memset(&rsp_fields, 0, sizeof rsp_fields);
    name                             = ble_svc_gap_device_name();
    rsp_fields.name                  = reinterpret_cast<const uint8_t *>(name);
    rsp_fields.name_len              = static_cast<uint8_t>(strlen(name));
    rsp_fields.name_is_complete      = 1;
    rsp_fields.tx_pwr_lvl_is_present = 1;
    rsp_fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting scan response data; rc=%d\n", rc);
        return;
    }

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER,
                           &adv_params, bleprph_gap_event, nullptr);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
    }
}

static int bleprph_gap_event(struct ble_gap_event *event, void * /*arg*/)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        MODLOG_DFLT(INFO, "connection %s; status=%d ",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            bleprph_print_conn_desc(&desc);
        }
        MODLOG_DFLT(INFO, "\n");
        if (event->connect.status != 0) {
            bleprph_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        MODLOG_DFLT(INFO, "disconnect; reason=%d ",
                    event->disconnect.reason);
        bleprph_print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        MODLOG_DFLT(INFO, "connection updated; status=%d ",
                    event->conn_update.status);
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        assert(rc == 0);
        bleprph_print_conn_desc(&desc);
        MODLOG_DFLT(INFO, "\n");
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        MODLOG_DFLT(INFO, "advertise complete; reason=%d",
                    event->adv_complete.reason);
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        MODLOG_DFLT(INFO, "encryption change event; status=%d ",
                    event->enc_change.status);
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        assert(rc == 0);
        bleprph_print_conn_desc(&desc);
        MODLOG_DFLT(INFO, "\n");
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        MODLOG_DFLT(INFO,
                    "notify_tx event; conn_handle=%d attr_handle=%d "
                    "status=%d is_indication=%d",
                    event->notify_tx.conn_handle,
                    event->notify_tx.attr_handle,
                    event->notify_tx.status,
                    event->notify_tx.indication);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        MODLOG_DFLT(INFO,
                    "subscribe event; conn_handle=%d attr_handle=%d "
                    "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                    event->subscribe.conn_handle,
                    event->subscribe.attr_handle,
                    event->subscribe.reason,
                    event->subscribe.prev_notify,
                    event->subscribe.cur_notify,
                    event->subscribe.prev_indicate,
                    event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        ESP_LOGI(tag, "PASSKEY_ACTION_EVENT started");
        struct ble_sm_io pkey = {0};

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action  = event->passkey.params.action;
            pkey.passkey = 123456;
            ESP_LOGI(tag, "Enter passkey %" PRIu32 " on the peer side",
                     pkey.passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.action        = event->passkey.params.action;
            pkey.numcmp_accept = 0;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
            static uint8_t tem_oob[16] = {0};
            pkey.action = event->passkey.params.action;
            for (int i = 0; i < 16; i++) { pkey.oob[i] = tem_oob[i]; }
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            pkey.action  = event->passkey.params.action;
            pkey.passkey = 0;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        return 0;
    }

    case BLE_GAP_EVENT_AUTHORIZE:
        MODLOG_DFLT(INFO,
                    "authorize event: conn_handle=%d attr_handle=%d is_read=%d",
                    event->authorize.conn_handle,
                    event->authorize.attr_handle,
                    event->authorize.is_read);
        event->authorize.out_response = BLE_GAP_AUTHORIZE_REJECT;
        return 0;

#if MYNEWT_VAL(BLE_CONN_SUBRATING)
    case BLE_GAP_EVENT_SUBRATE_CHANGE:
        MODLOG_DFLT(INFO,
                    "Subrate change event: conn_handle=%d status=%d factor=%d",
                    event->subrate_change.conn_handle,
                    event->subrate_change.status,
                    event->subrate_change.subrate_factor);
        return 0;
#endif
    }
    return 0;
}

static void bleprph_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

static void bleprph_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, nullptr);
    MODLOG_DFLT(INFO, "Device Address: ");
    print_addr(addr_val);
    MODLOG_DFLT(INFO, "\n");

    bleprph_advertise();
}

static void bleprph_host_task(void * /*param*/)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ============================================================
//  app_main
// ============================================================
extern "C" void ble_store_config_init(void) {}
extern "C" void app_main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ---- NVS ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- Internal temperature sensor ----
    initialize_internal_temp();
    xTaskCreate(internal_temp_monitor_task, "chip_temp_monitor",
                4096, nullptr, 5, nullptr);

    // ---- NimBLE init ----
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Failed to init nimble %d", ret);
        return;
    }

    ble_hs_cfg.reset_cb          = bleprph_on_reset;
    ble_hs_cfg.sync_cb           = bleprph_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_sc             = 0;

#if MYNEWT_VAL(BLE_GATTS)
    {
        int rc = gatt_svr_init();
        if (rc != 0) {
            MODLOG_DFLT(ERROR, "gatt_svr_init() failed with rc=%d\n", rc);
            return;
        }
    }
#endif

    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(device_name));
    ble_store_config_init();

    xTaskCreate(bleprph_host_task, "nimble_host", NIMBLE_HOST_STACK_SIZE,
                nullptr, 5, nullptr);

    // ---- MAX30102 init (once at boot) ----
    if (!max30102_init()) {
        ESP_LOGE(TAG, "MAX30102 init failed — check wiring!");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGE(TAG, "Sensor not found. Check: VIN=3.3V, GND, SDA=GPIO%d, SCL=GPIO%d",
                     I2C_SDA_PIN, I2C_SCL_PIN);
        }
    }

    ESP_LOGI(TAG, "Starting light-sleep measurement loop (~1s active / ~9s sleep)");

    // ============================================================
    //  Main light-sleep loop
    //
    //  Each iteration:
    //    1. Wake MAX30102, collect HR + SpO2 + Temp  (~1 s)
    //    2. Push results to BLE GATT characteristic
    //    3. Shut down MAX30102 LEDs
    //    4. Light sleep for the remainder of MEASUREMENT_PERIOD_US
    //       — BLE radio and FreeRTOS tasks stay alive during sleep
    //    5. Repeat forever
    // ============================================================
    while (true) {
        const int64_t cycle_start_us = esp_timer_get_time();
        measurement_t measurement    = {};

        if (collect_measurement(&measurement)) {
            print_measurement(&measurement);
            gatt_svr_set_measurement(
                measurement.heart_rate_bpm, measurement.heart_rate_valid,
                measurement.spo2_percent,   measurement.spo2_valid,
                measurement.temperature_c);
        } else {
            printf("Measurement failed\n");
        }

        // Calculate how long to sleep to keep the 10-second period
        const int64_t elapsed_us = esp_timer_get_time() - cycle_start_us;
        uint64_t sleep_time_us   = MIN_SLEEP_US;

        if (elapsed_us < static_cast<int64_t>(MEASUREMENT_PERIOD_US)) {
            sleep_time_us = static_cast<uint64_t>(
                static_cast<int64_t>(MEASUREMENT_PERIOD_US) - elapsed_us);
            if (sleep_time_us < MIN_SLEEP_US) {
                sleep_time_us = MIN_SLEEP_US;
            }
        }

        // Shut sensor down, light sleep, wake sensor back up
        sensor_light_sleep(sleep_time_us);
    }
}