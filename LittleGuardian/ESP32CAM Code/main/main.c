#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "ble.h"
#include "server.h"
#include "camera.h"
#include "CNN.h"

static const char *TAG = "MAIN";

void ble_store_config_init(void) { }

void cnn_task(void *arg)
{
    ESP_LOGI("CNN", "CNN task started");
    while (1) {
        run_inference();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    /* ---------- NVS ---------- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ---------- Camera ---------- */
    ESP_LOGI(TAG, "Initializing camera...");
    ESP_ERROR_CHECK(camera_init());
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* ---------- Allocate CNN buffers once ---------- */
    ESP_LOGI(TAG, "Allocating CNN buffers...");
    cnn_allocate_buffers();

    /* ---------- WiFi Manager ----------
     * - No saved credentials  → starts AP "BabyMonitor-Setup" + captive portal
     * - Saved credentials OK  → connects STA, starts HTTP servers on :80 / :81
     * - STA connection fails  → erases credentials, reboots into AP mode
     * NOTE: wifi_manager_start() calls esp_netif_init() and
     *       esp_event_loop_create_default() internally, so do NOT call
     *       them before this point.
     ----------------------------------------------------------------- */
    ESP_LOGI(TAG, "Starting WiFi manager...");
    wifi_manager_start();

    /* ---------- Start CNN task (lower priority, core 1) ---------- */
    xTaskCreatePinnedToCore(
        cnn_task,
        "cnn_task",
        8192,
        NULL,
        3,
        NULL,
        1
    );

    vTaskDelay(pdMS_TO_TICKS(500));
    ble_init();
}