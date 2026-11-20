#pragma once

#include "bb_config.h"

#include "esp_err.h"

#define BB_AP_PASSWORD "bambutton"

esp_err_t bb_wifi_start(const bb_config_t *cfg);
bool bb_wifi_sta_connected(void);
bool bb_wifi_ap_active(void);
void bb_wifi_get_ip(char *out, size_t out_len);
void bb_wifi_get_station(char *ssid, size_t ssid_len, char *ip, size_t ip_len, char *netmask, size_t netmask_len);

/* Write "-62 dBm, good" when the station is joined. Quality is excellent at
 * -50 dBm and above, good to -60, fair to -70, and weak below that. */
bool bb_wifi_format_signal(char *out, size_t out_len);

/* Join this station without restarting. The caller stores the settings first. */
esp_err_t bb_wifi_apply_station(const bb_config_t *cfg, bool *joined);
const char *bb_wifi_hostname(void);
