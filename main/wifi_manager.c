#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "uvc_source.h"
#include "wifi_manager.h"

static const char *TAG = "wifi";

#define RETRY_DELAY_MIN_MS 500
#define RETRY_DELAY_MAX_MS 10000

static esp_timer_handle_t s_retry_timer;
static volatile bool s_has_ip;
static volatile bool s_streaming_active;
static unsigned s_fail_count;
static uint32_t s_retry_delay_ms = RETRY_DELAY_MIN_MS;

static void update_power_save(void)
{
    // Keep modem sleep enabled at idle; disable it only while delivering live video.
    wifi_ps_type_t mode = (s_has_ip && s_streaming_active) ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM;
    esp_err_t err = esp_wifi_set_ps(mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps: %s", esp_err_to_name(err));
    }
}

void wifi_manager_set_streaming(bool active)
{
    s_streaming_active = active;
    update_power_save();
}

bool wifi_manager_has_ip(void)
{
    return s_has_ip;
}

static void retry_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        s_has_ip = false;
        uvc_source_set_network_ready(false);
        update_power_save();
        s_fail_count++;
        ESP_LOGW(TAG, "Disconnected (reason %d), attempt %u, retry in %u ms",
                 ev->reason, s_fail_count, (unsigned)s_retry_delay_ms);
        if (CONFIG_WROOMCAM_WIFI_MAX_RECONNECT > 0 && s_fail_count >= CONFIG_WROOMCAM_WIFI_MAX_RECONNECT) {
            ESP_LOGE(TAG, "Too many failed Wi-Fi attempts, restarting");
            esp_restart();
        }
        // A previous one-shot may still be pending if disconnect events arrive quickly.
        (void)esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_delay_ms * 1000);
        s_retry_delay_ms = s_retry_delay_ms * 2 > RETRY_DELAY_MAX_MS ? RETRY_DELAY_MAX_MS : s_retry_delay_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Got IP " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        s_has_ip = true;
        s_fail_count = 0;
        s_retry_delay_ms = RETRY_DELAY_MIN_MS;
        update_power_save();
        uvc_source_set_network_ready(true);
    }
}

#ifdef CONFIG_WROOMCAM_STATIC_IP
static esp_err_t apply_static_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t info = {0};
    esp_netif_dns_info_t dns = {0};
    esp_err_t err;

    if (esp_netif_str_to_ip4(CONFIG_WROOMCAM_IP_ADDR, &info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_WROOMCAM_IP_GATEWAY, &info.gw) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_WROOMCAM_IP_NETMASK, &info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_WROOMCAM_IP_DNS, &dns.ip.u_addr.ip4) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid static IP settings; keeping DHCP enabled");
        return ESP_ERR_INVALID_ARG;
    }
    dns.ip.type = ESP_IPADDR_TYPE_V4;

    err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not stop DHCP client: %s; keeping DHCP enabled", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_set_ip_info(netif, &info);
    if (err == ESP_OK) {
        err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not apply static network settings: %s; restoring DHCP", esp_err_to_name(err));
        esp_err_t dhcp_err = esp_netif_dhcpc_start(netif);
        if (dhcp_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not restart DHCP client: %s", esp_err_to_name(dhcp_err));
        }
        return err;
    }
    ESP_LOGI(TAG, "Static IP " IPSTR, IP2STR(&info.ip));
    return ESP_OK;
}
#endif

esp_err_t wifi_manager_start(void)
{
    if (strcmp(CONFIG_WROOMCAM_WIFI_SSID, "YOUR_WIFI_SSID") == 0 || CONFIG_WROOMCAM_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi SSID is still the placeholder. Create sdkconfig.secrets (see README).");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

#ifdef CONFIG_WROOMCAM_STATIC_IP
    (void)apply_static_ip(netif);  // parsing/application errors are logged; DHCP is retained/restored
#endif

    const esp_timer_create_args_t targs = {.callback = retry_timer_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_WROOMCAM_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_WROOMCAM_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = CONFIG_WROOMCAM_WIFI_PASSWORD[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_streaming_active = false;
    update_power_save();
    return ESP_OK;
}
