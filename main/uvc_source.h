#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "usb/uvc_host.h"

/** Install USB host + UVC driver and start the self-recovering supervisor task. */
esp_err_t uvc_source_start(void);

/** Enable/disable frame delivery. While disabled, frames are returned to the driver at once. */
void uvc_source_set_consumer(bool active);

/** Wait for the newest frame. Must be released with uvc_source_release_frame(). NULL on timeout. */
uvc_host_frame_t *uvc_source_get_frame(TickType_t timeout);

/** Give a frame obtained from uvc_source_get_frame() back to the driver. */
void uvc_source_release_frame(uvc_host_frame_t *frame);
