#include "camera_source.h"
#include "esp_log.h"

static const char *TAG = "camera";

#define PIN_PWDN  32
#define PIN_RESET -1
#define PIN_XCLK  0
#define PIN_SDA   26
#define PIN_SCL   27
#define PIN_D0    5
#define PIN_D1    18
#define PIN_D2    19
#define PIN_D3    21
#define PIN_D4    36
#define PIN_D5    39
#define PIN_D6    34
#define PIN_D7    35
#define PIN_VSYNC 25
#define PIN_HREF  23
#define PIN_PCLK  22

esp_err_t camera_source_start(void)
{
    camera_config_t cfg = {
        .pin_pwdn = PIN_PWDN,
        .pin_reset = PIN_RESET,
        .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = PIN_SDA,
        .pin_sccb_scl = PIN_SCL,
        .pin_d0 = PIN_D0,
        .pin_d1 = PIN_D1,
        .pin_d2 = PIN_D2,
        .pin_d3 = PIN_D3,
        .pin_d4 = PIN_D4,
        .pin_d5 = PIN_D5,
        .pin_d6 = PIN_D6,
        .pin_d7 = PIN_D7,
        .pin_vsync = PIN_VSYNC,
        .pin_href = PIN_HREF,
        .pin_pclk = PIN_PCLK,
        .xclk_freq_hz = CONFIG_WROOMCAM_CAM_XCLK_MHZ * 1000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = CONFIG_WROOMCAM_CAM_FRAME_SIZE,
        .jpeg_quality = CONFIG_WROOMCAM_CAM_JPEG_QUALITY,
        .fb_count = CONFIG_WROOMCAM_CAM_NUM_FRAME_BUFFERS,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "OV2640 ready (frame size %d, JPEG quality %d, %d PSRAM buffers)",
             CONFIG_WROOMCAM_CAM_FRAME_SIZE, CONFIG_WROOMCAM_CAM_JPEG_QUALITY,
             CONFIG_WROOMCAM_CAM_NUM_FRAME_BUFFERS);
    return ESP_OK;
}

camera_fb_t *camera_source_get_frame(TickType_t timeout)
{
    // esp_camera_fb_get() blocks up to its internal timeout (~2 s); the argument is kept for API symmetry.
    (void)timeout;
    return esp_camera_fb_get();
}

void camera_source_release_frame(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}
