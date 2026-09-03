#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Data structure received from BLE */
struct health_struc {
    uint8_t heart_rate;
    uint8_t o2;
    uint8_t battery;
    uint8_t padding;   // optional padding if needed
    float body_temp;
};

/* Shared data */
extern struct health_struc received_data;
extern bool ble_data_ready;

/* Initialize BLE and start host task */
void ble_init(void);

/* Callback to start scan after sync */
void ble_on_sync(void);
