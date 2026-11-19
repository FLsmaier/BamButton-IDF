#pragma once

#include "bb_config.h"

#include "esp_err.h"

#define BB_AP_PASSWORD "bambutton"

esp_err_t bb_wifi_start(const bb_config_t *cfg);
bool bb_wifi_sta_connected(void);
bool bb_wifi_ap_active(void);
void bb_wifi_get_ip(char *out, size_t out_len);
const char *bb_wifi_hostname(void);
