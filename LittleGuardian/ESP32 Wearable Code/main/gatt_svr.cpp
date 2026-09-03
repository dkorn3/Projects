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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/ans/ble_svc_ans.h"
#include "bleprph.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*** Maximum number of characteristics with the notify flag ***/
#define MAX_NOTIFY 5

// System UUID:        db1c343a-796b-46dc-b072-bfb3cc3b021b
static const ble_uuid128_t system_uuid =
    BLE_UUID128_INIT(0x1b, 0x02, 0x3b, 0xcc, 0xb3, 0xbf, 0x72, 0xb0,
                     0xdc, 0x46, 0x6b, 0x79, 0x3a, 0x34, 0x1c, 0xdb);

// Health Sensors UUID: 43694954-33cc-4dce-ba5b-bd64cae6d117
static const ble_uuid128_t current_data_uuid =
    BLE_UUID128_INIT(0x17, 0xd1, 0xe6, 0xca, 0x64, 0xbd, 0x5b, 0xba,
                     0xce, 0x4d, 0xcc, 0x33, 0x54, 0x49, 0x69, 0x43);

// ---------------------------------------------------------------------------
//  Shared sensor data
//  Written by main.cpp via gatt_svr_set_measurement(), read on every BLE read.
// ---------------------------------------------------------------------------

struct health_struc {
    uint8_t heart_rate;  // BPM
    uint8_t o2;          // SpO2 %
    uint8_t battery;     // % (placeholder — not yet wired to real ADC)
    uint8_t padding;     // always 0
    float   body_temp;   // °C from MAX30102 die temperature
};

static struct health_struc current_data;
static SemaphoreHandle_t   s_data_mutex;

// Called from main.cpp after every successful collect_measurement()
void gatt_svr_set_measurement(int32_t heart_rate_bpm, bool hr_valid,
                               int32_t spo2_percent,   bool spo2_valid,
                               float   temperature_c)
{
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current_data.heart_rate = hr_valid   ? (uint8_t)heart_rate_bpm : 0;
        current_data.o2         = spo2_valid ? (uint8_t)spo2_percent   : 0;
        current_data.battery    = 100;   // TODO: wire real battery ADC
        current_data.padding    = 0;
        current_data.body_temp  = temperature_c;
        xSemaphoreGive(s_data_mutex);
    }
}

// ---------------------------------------------------------------------------
//  GATT access callback
// ---------------------------------------------------------------------------

static int gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (const ble_uuid_t *)&system_uuid,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Health Sensors characteristic — read returns live sensor data
                .uuid      = (const ble_uuid_t *)&current_data_uuid,
                .access_cb = gatt_svc_access,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

/*
 * Data format on the wire (8 bytes total):
 *   HEART RATE (8-bit) | O2 (8-bit) | BATTERY (8-bit) | PADDING (8-bit) | BODY TEMP (32-bit float)
 */
static int gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    const ble_uuid_t *uuid = ctxt->chr->uuid;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (ble_uuid_cmp(uuid, &current_data_uuid.u) == 0) {
            struct health_struc snapshot;
            if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                snapshot = current_data;
                xSemaphoreGive(s_data_mutex);
            } else {
                // Mutex timeout — return zeroed struct rather than stale/torn data
                memset(&snapshot, 0, sizeof(snapshot));
            }
            return os_mbuf_append(ctxt->om, &snapshot, sizeof(snapshot)) == 0
                       ? 0
                       : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

// ---------------------------------------------------------------------------
//  Registration callback
// ---------------------------------------------------------------------------

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        MODLOG_DFLT(DEBUG, "registered service %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        MODLOG_DFLT(DEBUG, "registering characteristic %s with "
                    "def_handle=%d val_handle=%d\n",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle,
                    ctxt->chr.val_handle);
        break;
    case BLE_GATT_REGISTER_OP_DSC:
        MODLOG_DFLT(DEBUG, "registering descriptor %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
        break;
    default:
        assert(0);
        break;
    }
}

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------

int gatt_svr_init(void)
{
    s_data_mutex = xSemaphoreCreateMutex();
    assert(s_data_mutex != NULL);

    memset(&current_data, 0, sizeof(current_data));

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_ans_init();

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) return rc;

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) return rc;

    return 0;
}