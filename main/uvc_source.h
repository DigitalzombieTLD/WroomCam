#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "usb/uvc_host.h"

/** Install USB host + UVC driver and start the self-recovering supervisor task. */
esp_err_t uvc_source_start(void);

/** Enable/disable frame delivery based on whether an MJPEG viewer is connected. */
void uvc_source_set_consumer(bool active);

/** Tell the UVC supervisor whether station Wi-Fi has an IPv4 connection. */
void uvc_source_set_network_ready(bool ready);

/** Wait for the newest frame. Must be released with uvc_source_release_frame(). NULL on timeout. */
uvc_host_frame_t *uvc_source_get_frame(TickType_t timeout);

/** Give a frame obtained from uvc_source_get_frame() back to the driver. */
void uvc_source_release_frame(uvc_host_frame_t *frame);
