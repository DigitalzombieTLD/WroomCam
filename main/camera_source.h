#pragma once

#include "esp_camera.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/** Initialise the OV2640 (AI-Thinker ESP32-CAM pin map) with JPEG output and PSRAM frame buffers. */
esp_err_t camera_source_start(void);

/** Wait for a JPEG frame. Must be returned with camera_source_release_frame(). NULL on timeout. */
camera_fb_t *camera_source_get_frame(TickType_t timeout);

/** Give a frame obtained from camera_source_get_frame() back to the driver. */
void camera_source_release_frame(camera_fb_t *fb);
