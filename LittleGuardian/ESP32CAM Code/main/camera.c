#include "camera.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CAMERA";

#define DST_W 64
#define DST_H 64
#define UINT8_FRAME_SIZE (DST_W * DST_H)

/* =========================
 * RGB565 → Grayscale
 * ========================= */
static inline uint8_t rgb565_to_gray(uint16_t pixel)
{
    pixel = (pixel >> 8) | (pixel << 8);
    uint8_t r = ((pixel >> 11) & 0x1F) << 3;
    uint8_t g = ((pixel >> 5)  & 0x3F) << 2;
    uint8_t b = ( pixel        & 0x1F) << 3;
return (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
}

/* =========================
 * Camera initialization
 *
 * OV3660-specific notes:
 *  - XCLK: 20MHz is fine for OV3660 (unlike OV2640 which struggles).
 *    OV3660 uses an internal PLL so it is less sensitive to XCLK.
 *  - The FB-SIZE mismatch on OV3660 at QVGA RGB565 is a known driver
 *    quirk — the sensor outputs 320x216 instead of 320x240 in some
 *    firmware versions. We handle this safely by reading fb->width/height.
 *  - sensor_t settings: OV3660 does NOT support set_sharpness,
 *    set_denoise, or set_special_effect the same way OV2640 does.
 *    Stick to the settings that are confirmed to work.
 *  - Warm-up: OV3660 AEC/AGC needs ~10 frames (more than OV2640)
 *    to settle in a new lighting environment.
 * ========================= */
esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn       = -1,
        .pin_reset      = -1,
        .pin_xclk       = 10,
        .pin_sccb_sda   = 40,
        .pin_sccb_scl   = 39,

        .pin_d7         = 48,
        .pin_d6         = 11,
        .pin_d5         = 12,
        .pin_d4         = 14,
        .pin_d3         = 16,
        .pin_d2         = 18,
        .pin_d1         = 17,
        .pin_d0         = 15,

        .pin_vsync      = 38,
        .pin_href       = 47,
        .pin_pclk       = 13,

        // OV3660 uses an internal PLL — 20MHz XCLK is correct and stable.
        // (OV2640 needed 16MHz, OV3660 does not have that constraint.)
        .xclk_freq_hz   = 20000000,

        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,

        .pixel_format   = PIXFORMAT_RGB565,
        .frame_size     = FRAMESIZE_QVGA,   // 320x240

        .fb_count       = 3,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_LATEST
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        // OV3660 confirmed-working sensor settings
        s->set_brightness(s, 0);        // -2 to 2  (0 = normal)
        s->set_contrast(s, 0);          // -2 to 2
        s->set_saturation(s, 0);        // -2 to 2
        s->set_whitebal(s, 1);          // auto white balance on
        s->set_awb_gain(s, 1);          // AWB gain on
        s->set_wb_mode(s, 0);           // 0=auto, 1=sunny, 2=cloudy, 3=office, 4=home
        s->set_exposure_ctrl(s, 1);     // auto exposure on
        s->set_aec2(s, 1);              // AEC DSP on (OV3660 benefits from this)
        s->set_ae_level(s, 0);          // -2 to 2 exposure compensation
        s->set_gain_ctrl(s, 1);         // auto gain on
        s->set_gainceiling(s, GAINCEILING_4X); // lower than OV2640 — OV3660 is more sensitive
        s->set_bpc(s, 1);               // black pixel correction
        s->set_wpc(s, 1);               // white pixel correction
        s->set_raw_gma(s, 1);           // gamma correction
        s->set_lenc(s, 1);              // lens correction
        s->set_hmirror(s, 0);           // flip horizontal if needed
        s->set_vflip(s, 0);             // flip vertical if needed
        s->set_dcw(s, 1);              // downsize enable
        s->set_colorbar(s, 0);          // no test colorbar
    }

    ESP_LOGI(TAG, "Camera initialized (OV3660, RGB565, QVGA)");

    // OV3660 needs more warm-up frames than OV2640 for AEC/AGC to settle.
    // Discard 10 frames before trusting output for inference.
    ESP_LOGI(TAG, "Camera warm-up (discarding 10 frames)...");
    for (int i = 0; i < 10; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            ESP_LOGD(TAG, "Warm-up frame %d: %dx%d (%zu bytes)",
                     i + 1, fb->width, fb->height, fb->len);
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Camera warm-up done");

    return ESP_OK;
}

/* =========================
 * Raw frame grab (RGB565)
 * ========================= */
esp_err_t camera_get_frame(camera_fb_t **fb_out)
{
    if (!fb_out) return ESP_ERR_INVALID_ARG;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Frame capture failed");
        return ESP_FAIL;
    }

    if (fb->format != PIXFORMAT_RGB565) {
        ESP_LOGE(TAG, "Unexpected format: %d", fb->format);
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }

    *fb_out = fb;
    return ESP_OK;
}

/* =========================
 * ML frame: 64x64 UINT8
 *
 * Uses fb->width / fb->height (actual delivered dimensions) so the
 * resize math is always correct even if the OV3660 delivers 320x216
 * instead of 320x240 (known driver quirk with QVGA RGB565).
 * ========================= */
esp_err_t camera_get_frame_uint8(uint8_t *out, size_t len)
{
    if (!out || len < UINT8_FRAME_SIZE) return ESP_ERR_INVALID_ARG;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Frame capture failed");
        return ESP_FAIL;
    }

    if (fb->format != PIXFORMAT_RGB565) {
        ESP_LOGE(TAG, "Unexpected format: %d", fb->format);
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }

    int actual_w = (int)fb->width;
    int actual_h = (int)fb->height;

    ESP_LOGD(TAG, "Frame: %dx%d  buf=%zu bytes", actual_w, actual_h, fb->len);

    uint16_t *src = (uint16_t *)fb->buf;

 for (int y = 0; y < DST_H; y++) {
    for (int x = 0; x < DST_W; x++) {
        int x0 = (x * actual_w) / DST_W;
        int x1 = ((x + 1) * actual_w) / DST_W;
        int y0 = (y * actual_h) / DST_H;
        int y1 = ((y + 1) * actual_h) / DST_H;

        uint32_t sum = 0, count = 0;
        for (int sy = y0; sy < y1; sy++) {
            for (int sx = x0; sx < x1; sx++) {
                sum += rgb565_to_gray(src[sy * actual_w + sx]);
                count++;
            }
        }
        out[y * DST_W + x] = (uint8_t)(sum / count);
    }
}
    esp_camera_fb_return(fb);
    return ESP_OK;
}