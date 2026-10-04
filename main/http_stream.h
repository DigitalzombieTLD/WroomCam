#pragma once

#include "esp_err.h"

/** Start the HTTP server: "/" (info page) and "/stream" (multipart MJPEG; one viewer at a time). */
esp_err_t http_stream_start(void);
