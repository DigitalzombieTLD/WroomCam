#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "uvc_source.h"
#include "http_stream.h"

static const char *TAG = "wroomcam";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Install USB first so Wi-Fi events can safely notify the camera supervisor.
    ESP_ERROR_CHECK(uvc_source_start());
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(http_stream_start());

    ESP_LOGI(TAG, "Started; stream will be at http://<device-ip>:%d/stream", CONFIG_WROOMCAM_HTTP_PORT);
}
