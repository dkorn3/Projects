#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  WiFi Manager — call once from app_main after nvs_flash_init().
 *
 * Behaviour:
 *   • No saved credentials  → AP mode ("BabyMonitor-Setup") + captive portal
 *                             at http://192.168.4.1  for first-time setup.
 *   • Saved credentials     → STA mode; on success starts the HTTP servers
 *                             (port 80: dashboard + /data + /forget,
 *                              port 81: MJPEG stream).
 *   • STA connection fails  → credentials erased, device reboots into AP mode.
 *
 * Internally calls esp_netif_init() and esp_event_loop_create_default(),
 * so do NOT call those before wifi_manager_start().
 */
void wifi_manager_start(void);

/**
 * @brief  Stop all HTTP servers and WiFi.  Optional — call on fatal error.
 */
void wifi_manager_stop(void);

#ifdef __cplusplus
}
#endif