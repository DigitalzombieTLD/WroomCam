#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_stream.h"
#include "uvc_source.h"

static const char *TAG = "http";

#define BOUNDARY "wroomcamframe"
#define CONTENT_TYPE "multipart/x-mixed-replace;boundary=" BOUNDARY
#define WAIT_SLICE_MS 1000

static esp_err_t index_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><title>WroomCam</title><body style=\"margin:0;background:#000\">"
        "<img src=\"/stream\" style=\"max-width:100%\"></body>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static bool valid_jpeg(const uvc_host_frame_t *f)
{
    return f->data_len > 4 && f->data[0] == 0xFF && f->data[1] == 0xD8;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    // esp_http_server runs handlers on one task, so this serves a single viewer at a time;
    // further connections queue until the current stream ends.
    ESP_LOGI(TAG, "Stream client connected");
    httpd_resp_set_type(req, CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t err = ESP_OK;
    unsigned sent = 0;
    int idle_ms = 0;
    uvc_source_set_consumer(true);

    while (err == ESP_OK) {
        uvc_host_frame_t *frame = uvc_source_get_frame(pdMS_TO_TICKS(WAIT_SLICE_MS));
        if (!frame) {
            idle_ms += WAIT_SLICE_MS;
            if (idle_ms >= CONFIG_WROOMCAM_HTTP_FRAME_WAIT_S * 1000) {
                ESP_LOGW(TAG, "No frames for %d s, closing stream response", idle_ms / 1000);
                break;
            }
            continue;
        }
        idle_ms = 0;

        if (valid_jpeg(frame)) {
            char hdr[128];
            int n = snprintf(hdr, sizeof(hdr),
                             "%s--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                             sent ? "\r\n" : "", (unsigned)frame->data_len);
            err = httpd_resp_send_chunk(req, hdr, n);
            if (err == ESP_OK) {
                err = httpd_resp_send_chunk(req, (const char *)frame->data, frame->data_len);  // zero copy
            }
            sent++;
        }
        uvc_source_release_frame(frame);
    }

    uvc_source_set_consumer(false);
    ESP_LOGI(TAG, "Stream client gone (%u frames sent, %s)", sent, esp_err_to_name(err));
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : ESP_FAIL;
}

esp_err_t http_stream_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_WROOMCAM_HTTP_PORT;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.core_id = 0;
    cfg.send_wait_timeout = CONFIG_WROOMCAM_HTTP_SEND_TIMEOUT_S;
    cfg.recv_wait_timeout = 5;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler};
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &stream_uri);
    return ESP_OK;
}
