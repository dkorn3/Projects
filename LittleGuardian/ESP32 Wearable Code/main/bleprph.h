#ifndef H_BLEPRPH_
#define H_BLEPRPH_

#include <stdbool.h>
#include "nimble/ble.h"
#include "modlog/modlog.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ble_hs_cfg;
struct ble_gatt_register_ctxt;

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);
int  gatt_svr_init(void);

void gatt_svr_set_measurement(int32_t heart_rate_bpm, bool hr_valid,
                               int32_t spo2_percent,   bool spo2_valid,
                               float   temperature_c);

#ifdef __cplusplus
}
#endif

#endif /* H_BLEPRPH_ */