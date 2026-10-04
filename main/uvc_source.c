#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "uvc_source.h"

static const char *TAG = "uvc";

#define LOW_RES_W 320
#define LOW_RES_H 240
#define OPEN_TIMEOUT_MS 2000
#define MONITOR_PERIOD_MS 250
#define HEALTHY_RUN_MS 10000
#define MAX_TRANSFER_ERRORS 10
#define RELEASE_WAIT_MS ((CONFIG_WROOMCAM_HTTP_SEND_TIMEOUT_S + 2) * 1000)
#define IDLE_RECHECK_MS 1000

enum {
    EV_DISCONNECTED = BIT0,
    EV_BROKEN = BIT1,
    EV_STATE_CHANGED = BIT2,
};

static EventGroupHandle_t s_events;
static QueueHandle_t s_queue;  // holds at most the single newest frame
static uvc_host_stream_hdl_t s_stream;
static atomic_bool s_consumer;
static atomic_bool s_network_ready;
static atomic_bool s_accepting;
static atomic_bool s_device_seen;
static atomic_int s_outstanding;  // frames kept from the driver (queued or held by the HTTP handler)
static atomic_uint s_transfer_errors;
static atomic_ullong s_last_frame_us;
static atomic_uint s_frames;
static atomic_uint s_dropped;

static unsigned s_width, s_height;
static float s_fps;
static size_t s_frame_size;
static uint32_t s_frame_caps;

static void drain_queue(void);

static bool stream_is_allowed(void)
{
    return atomic_load(&s_consumer) && atomic_load(&s_network_ready);
}

void uvc_source_set_consumer(bool active)
{
    atomic_store(&s_consumer, active);
    if (!active && s_queue) {
        drain_queue();
    }
    if (s_events) {
        xEventGroupSetBits(s_events, EV_STATE_CHANGED);
    }
}

void uvc_source_set_network_ready(bool ready)
{
    atomic_store(&s_network_ready, ready);
    if (s_events) {
        xEventGroupSetBits(s_events, EV_STATE_CHANGED);
    }
}

uvc_host_frame_t *uvc_source_get_frame(TickType_t timeout)
{
    uvc_host_frame_t *frame = NULL;
    if (xQueueReceive(s_queue, &frame, timeout) != pdTRUE) {
        return NULL;
    }
    return frame;
}

static void return_frame(uvc_host_frame_t *frame)
{
    if (s_stream && uvc_host_frame_return(s_stream, frame) != ESP_OK) {
        ESP_LOGW(TAG, "uvc_host_frame_return failed");
    }
    atomic_fetch_sub(&s_outstanding, 1);
}

void uvc_source_release_frame(uvc_host_frame_t *frame)
{
    if (frame) {
        return_frame(frame);
    }
}

static void drain_queue(void)
{
    uvc_host_frame_t *frame;
    while (xQueueReceive(s_queue, &frame, 0) == pdTRUE) {
        return_frame(frame);
    }
}

// Runs in the UVC driver task: must not block. Keeps only the newest frame (zero copy).
static bool frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    atomic_store(&s_last_frame_us, (unsigned long long)esp_timer_get_time());
    atomic_fetch_add(&s_frames, 1);
    atomic_store(&s_transfer_errors, 0);

    if (!atomic_load(&s_consumer) || !atomic_load(&s_network_ready) ||
        !atomic_load(&s_accepting) || frame->data_len == 0) {
        return true;  // driver reuses the buffer immediately
    }

    uvc_host_frame_t *f = (uvc_host_frame_t *)frame;
    atomic_fetch_add(&s_outstanding, 1);
    if (xQueueSend(s_queue, &f, 0) != pdTRUE) {
        uvc_host_frame_t *old;
        if (xQueueReceive(s_queue, &old, 0) == pdTRUE) {
            return_frame(old);  // slow consumer: drop the stale frame, keep the newest
            atomic_fetch_add(&s_dropped, 1);
        }
        if (xQueueSend(s_queue, &f, 0) != pdTRUE) {
            atomic_fetch_sub(&s_outstanding, 1);
            return true;
        }
    }
    return false;  // ownership retained until uvc_source_release_frame()
}

static void stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB transfer error: %s", esp_err_to_name(event->transfer_error.error));
        if (atomic_fetch_add(&s_transfer_errors, 1) + 1 >= MAX_TRANSFER_ERRORS) {
            xEventGroupSetBits(s_events, EV_BROKEN);
        }
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "Camera disconnected");
        atomic_store(&s_device_seen, false);
        xEventGroupSetBits(s_events, EV_DISCONNECTED);
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "Frame buffer overflow: frame larger than buffer, raise WROOMCAM_CAM_FRAME_BUF_KB");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGD(TAG, "Frame dropped: no free buffer (consumer too slow)");
        break;
    default:
        break;
    }
}

static void driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        ESP_LOGI(TAG, "UVC device connected: addr %u, stream index %u, %u advertised frame modes",
                 event->device_connected.dev_addr, event->device_connected.uvc_stream_index,
                 (unsigned)event->device_connected.frame_info_num);
        atomic_store(&s_device_seen, true);
    }
}

static void usb_lib_task(void *arg)
{
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void choose_mode(void)
{
    s_width = CONFIG_WROOMCAM_CAM_WIDTH;
    s_height = CONFIG_WROOMCAM_CAM_HEIGHT;
    s_fps = CONFIG_WROOMCAM_CAM_FPS;

    bool psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    if (!psram) {
        ESP_LOGW(TAG, "No PSRAM detected; frame buffers will use internal RAM");
#ifdef CONFIG_WROOMCAM_NO_PSRAM_LOW_RES
        if (s_width * s_height > LOW_RES_W * LOW_RES_H) {
            ESP_LOGW(TAG, "Falling back to %dx%d (configured %ux%u)", LOW_RES_W, LOW_RES_H, s_width, s_height);
            s_width = LOW_RES_W;
            s_height = LOW_RES_H;
        }
#endif
    }

    size_t kb = CONFIG_WROOMCAM_CAM_FRAME_BUF_KB;
    s_frame_size = kb ? kb * 1024 : (size_t)s_width * s_height / 4;
    if (!kb && s_frame_size < 32 * 1024) {
        s_frame_size = 32 * 1024;
    }
    s_frame_caps = psram ? MALLOC_CAP_SPIRAM : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Mode: MJPEG %ux%u @ %.0f fps, %d x %u KiB frame buffers in %s",
             s_width, s_height, s_fps, CONFIG_WROOMCAM_CAM_NUM_FRAME_BUFFERS,
             (unsigned)(s_frame_size / 1024), psram ? "PSRAM" : "internal RAM");
}

// Returns true if the stream was closed cleanly.
static bool teardown_stream(uvc_host_stream_hdl_t stream, bool disconnected)
{
    atomic_store(&s_accepting, false);
    if (!disconnected) {
        esp_err_t err = uvc_host_stream_stop(stream);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "stream_stop: %s", esp_err_to_name(err));
        }
    }

    // The HTTP handler may still hold a frame; wait (bounded) until all are returned.
    int waited = 0;
    drain_queue();
    while (atomic_load(&s_outstanding) > 0 && waited < RELEASE_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
        drain_queue();
    }
    if (atomic_load(&s_outstanding) > 0) {
        ESP_LOGE(TAG, "Frames not released in time, cannot close stream safely");
        return false;
    }

    esp_err_t err = uvc_host_stream_close(stream);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream_close: %s", esp_err_to_name(err));
        return false;
    }
    s_stream = NULL;
    return true;
}

static void restart_chip(const char *why)
{
    ESP_LOGE(TAG, "Unrecoverable (%s): restarting", why);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void wait_for_state_change(TickType_t timeout)
{
    xEventGroupWaitBits(s_events, EV_STATE_CHANGED, pdTRUE, pdFALSE, timeout);
}

static void supervisor_task(void *arg)
{
    const uvc_host_stream_config_t cfg = {
        .event_cb = stream_event_cb,
        .frame_cb = frame_cb,
        .user_ctx = NULL,
        .usb = {.dev_addr = UVC_HOST_ANY_DEV_ADDR, .vid = UVC_HOST_ANY_VID, .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = 0},
        .vs_format = {.h_res = s_width, .v_res = s_height, .fps = s_fps, .format = UVC_VS_FORMAT_MJPEG},
        .advanced = {.number_of_frame_buffers = CONFIG_WROOMCAM_CAM_NUM_FRAME_BUFFERS,
                     .frame_size = s_frame_size,
                     .frame_heap_caps = s_frame_caps,
                     .number_of_urbs = 3,
                     .urb_size = 0},
    };

    unsigned failures = 0;
    uint32_t backoff_ms = CONFIG_WROOMCAM_BACKOFF_MIN_MS;
    unsigned wait_logs = 0;

    while (true) {
        // Do not open/start UVC until an HTTP viewer has requested the stream and Wi-Fi has an IP.
        while (!stream_is_allowed()) {
            wait_for_state_change(portMAX_DELAY);
        }
        xEventGroupClearBits(s_events, EV_DISCONNECTED | EV_BROKEN);
        atomic_store(&s_transfer_errors, 0);

        uvc_host_stream_hdl_t stream = NULL;
        esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(OPEN_TIMEOUT_MS), &stream);
        if (err == ESP_OK) {
            s_stream = stream;
            atomic_store(&s_device_seen, true);
            err = uvc_host_stream_start(stream);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "stream_start failed: %s (camera may not support this mode)", esp_err_to_name(err));
                if (!teardown_stream(stream, false)) {
                    restart_chip("close after failed start");
                }
            }
        }

        if (err != ESP_OK) {
            if (!stream_is_allowed()) {
                continue;
            }
            if (stream == NULL && !atomic_load(&s_device_seen)) {
                if (wait_logs++ % 5 == 0) {
                    ESP_LOGI(TAG, "Waiting for UVC camera on the USB-OTG port...");
                }
                wait_for_state_change(pdMS_TO_TICKS(IDLE_RECHECK_MS));
                continue;
            }
            failures++;
            ESP_LOGE(TAG, "Stream open/start failed (%s), failure %u/%d, retry in %u ms",
                     esp_err_to_name(err), failures, CONFIG_WROOMCAM_MAX_FAILURES, (unsigned)backoff_ms);
            if (failures >= CONFIG_WROOMCAM_MAX_FAILURES) {
                restart_chip("too many stream failures");
            }
            wait_for_state_change(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2 > CONFIG_WROOMCAM_BACKOFF_MAX_MS ? CONFIG_WROOMCAM_BACKOFF_MAX_MS : backoff_ms * 2;
            continue;
        }

        wait_logs = 0;
        int64_t started_us = esp_timer_get_time();
        atomic_store(&s_last_frame_us, (unsigned long long)started_us);
        unsigned frames_at_start = atomic_load(&s_frames);
        atomic_store(&s_accepting, true);
        ESP_LOGI(TAG, "UVC capture started for active MJPEG client");

        const char *reason = NULL;
        bool disconnected = false;
        bool idle_shutdown = false;
        while (!reason) {
            EventBits_t bits = xEventGroupWaitBits(s_events,
                                                   EV_DISCONNECTED | EV_BROKEN | EV_STATE_CHANGED,
                                                   pdTRUE, pdFALSE,
                                                   pdMS_TO_TICKS(MONITOR_PERIOD_MS));
            if (bits & EV_DISCONNECTED) {
                reason = "camera disconnected";
                disconnected = true;
            } else if (bits & EV_BROKEN) {
                reason = "repeated USB transfer errors";
            } else if ((bits & EV_STATE_CHANGED) && !stream_is_allowed()) {
                reason = atomic_load(&s_consumer) ? "Wi-Fi unavailable" : "no MJPEG clients";
                idle_shutdown = true;
            } else if ((esp_timer_get_time() - (int64_t)atomic_load(&s_last_frame_us)) / 1000 >
                       CONFIG_WROOMCAM_STALL_TIMEOUT_MS) {
                reason = "stream stalled (no frames)";
            }
        }
        ESP_LOGW(TAG, "Stopping UVC capture: %s (%u frames, %u dropped total)", reason,
                 atomic_load(&s_frames) - frames_at_start, atomic_load(&s_dropped));

        bool delivered = atomic_load(&s_frames) != frames_at_start;
        bool healthy = delivered && (esp_timer_get_time() - started_us) / 1000 >= HEALTHY_RUN_MS;
        if (!teardown_stream(stream, disconnected)) {
            restart_chip("stream close failed");
        }

        if (idle_shutdown || disconnected || healthy) {
            failures = 0;
            backoff_ms = CONFIG_WROOMCAM_BACKOFF_MIN_MS;
        } else {
            failures++;
            if (failures >= CONFIG_WROOMCAM_MAX_FAILURES) {
                restart_chip("too many stream failures");
            }
            ESP_LOGW(TAG, "Failure %u/%d, retry in %u ms", failures, CONFIG_WROOMCAM_MAX_FAILURES, (unsigned)backoff_ms);
            wait_for_state_change(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms * 2 > CONFIG_WROOMCAM_BACKOFF_MAX_MS ? CONFIG_WROOMCAM_BACKOFF_MAX_MS : backoff_ms * 2;
        }
    }
}

esp_err_t uvc_source_start(void)
{
    choose_mode();

    s_events = xEventGroupCreate();
    s_queue = xQueueCreate(1, sizeof(uvc_host_frame_t *));
    if (!s_events || !s_queue) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_cfg = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 3072, NULL, 2, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const uvc_host_driver_config_t drv_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 1,
        .create_background_task = true,
        .event_cb = driver_event_cb,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(uvc_host_install(&drv_cfg));

    if (xTaskCreatePinnedToCore(supervisor_task, "uvc_super", 4096, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
