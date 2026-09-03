#ifndef CAMERA_H
#define CAMERA_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include "esp_camera.h"
#define CAPTURE_WIDTH  64
#define CAPTURE_HEIGHT 64
#define GRAY_FRAME_SIZE (CAPTURE_WIDTH * CAPTURE_HEIGHT)

esp_err_t camera_init(void);
esp_err_t camera_get_frame(camera_fb_t **fb_out);
esp_err_t camera_get_frame_uint8(uint8_t *out, size_t len);
#endif // CAMERA_H