#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Start Wi-Fi station (DHCP or static IPv4, per Kconfig). Non-blocking; reconnects automatically. */
esp_err_t wifi_manager_start(void);

/** True once an IPv4 address is assigned. */
bool wifi_manager_has_ip(void);
