#include "bb_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

static const char *TAG = "bb.wifi";

#define BB_WIFI_CONNECTED_BIT BIT0
#define BB_WIFI_DROPPED_BIT BIT1
#define BB_WIFI_ATTEMPT_MS 10000
#define BB_WIFI_RETRY_US (30LL * 1000 * 1000)
#define BB_AP_HOLD_US (60LL * 1000 * 1000)

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static char s_hostname[32];
static char s_sta_ip[16];
static char s_sta_netmask[16];
static wifi_config_t s_sta_cfg;
static wifi_config_t s_ap_cfg;
static volatile bool s_auto_reconnect;
static volatile bool s_dropping;
static volatile bool s_ap_active;
static bool s_have_ssid;
static int64_t s_next_attempt_us;
static int64_t s_connected_since_us;

static void copy_ip(const esp_ip4_addr_t *addr)
{
    snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(addr));
}

static void copy_netmask(const esp_ip4_addr_t *addr)
{
    snprintf(s_sta_netmask, sizeof(s_sta_netmask), IPSTR, IP2STR(addr));
}

static void wifi_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void wifi_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void copy_ssid(char *out, size_t out_len, const uint8_t *ssid, uint8_t ssid_len)
{
    if (out_len == 0) {
        return;
    }
    size_t n = ssid_len;
    if (n >= out_len) {
        n = out_len - 1;
    }
    if (n == 0) {
        memcpy(out, s_sta_cfg.sta.ssid, 32);
        out[out_len - 1] = '\0';
        return;
    }
    memcpy(out, ssid, n);
    out[n] = '\0';
}

static void quiet_wifi_logs(void)
{
    /* The driver logs state changes, beacons, and buffer setup at INFO.
     * This component logs the SSID, the join, and the address. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
}

static const char *disconnect_reason(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
        return "unspecified";
    case WIFI_REASON_AUTH_EXPIRE:
        return "authentication expired";
    case WIFI_REASON_AUTH_LEAVE:
        return "left the network";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "inactivity";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "too many stations";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA:
        return "not authenticated";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:
        return "not associated";
    case WIFI_REASON_ASSOC_LEAVE:
        return "deauthenticated";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "associated but not authenticated";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "poor power capability";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "unsupported channel";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:
        return "BSS transition";
    case WIFI_REASON_IE_INVALID:
        return "invalid information element";
    case WIFI_REASON_MIC_FAILURE:
        return "MIC failure";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4-way handshake timeout";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "group key timeout";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "handshake element mismatch";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "invalid group cipher";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "invalid pairwise cipher";
    case WIFI_REASON_AKMP_INVALID:
        return "invalid authentication method";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "unsupported security version";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "invalid security capability";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802.1X authentication failed";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "cipher suite rejected";
    case WIFI_REASON_TIMEOUT:
        return "timeout";
    case WIFI_REASON_PEER_INITIATED:
        return "peer disconnected";
    case WIFI_REASON_AP_INITIATED:
        return "access point disconnected";
    case WIFI_REASON_MISSING_ACKS:
        return "missing acknowledgements";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout";
    case WIFI_REASON_NO_AP_FOUND:
        return "access point not found";
    case WIFI_REASON_AUTH_FAIL:
        return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
    case WIFI_REASON_AP_TSF_RESET:
        return "access point reset its clock";
    case WIFI_REASON_ROAMING:
        return "roaming";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "no access point with compatible security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "no access point in the allowed security mode";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "access point signal too weak";
    default:
        return NULL;
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_auto_reconnect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = data;
        char ssid[33];
        copy_ssid(ssid, sizeof(ssid), event->ssid, event->ssid_len);
        ESP_LOGI(TAG, "Station connected to \"%s\"", ssid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        char ssid[33];
        copy_ssid(ssid, sizeof(ssid), event->ssid, event->ssid_len);
        s_sta_ip[0] = '\0';
        s_sta_netmask[0] = '\0';
        xEventGroupClearBits(s_events, BB_WIFI_CONNECTED_BIT);
        const char *why = disconnect_reason(event->reason);
        if (why != NULL) {
            ESP_LOGI(TAG, "Station disconnected from \"%s\": %s", ssid, why);
        } else {
            ESP_LOGI(TAG, "Station disconnected from \"%s\": reason %u", ssid, event->reason);
        }
        if (s_dropping) {
            xEventGroupSetBits(s_events, BB_WIFI_DROPPED_BIT);
        } else if (s_auto_reconnect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        s_ap_active = true;
        ESP_LOGI(TAG, "Setup AP %s password %s at 192.168.4.1", s_hostname, BB_AP_PASSWORD);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        s_ap_active = false;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        copy_ip(&event->ip_info.ip);
        copy_netmask(&event->ip_info.netmask);
        xEventGroupSetBits(s_events, BB_WIFI_CONNECTED_BIT);
        char signal[32];
        if (bb_wifi_format_signal(signal, sizeof(signal))) {
            ESP_LOGI(TAG, "Station IP %s netmask %s, signal %s", s_sta_ip, s_sta_netmask, signal);
        } else {
            ESP_LOGI(TAG, "Station IP %s netmask %s", s_sta_ip, s_sta_netmask);
        }
    }
}

static void fill_ap_config(void)
{
    memset(&s_ap_cfg, 0, sizeof(s_ap_cfg));
    strlcpy((char *)s_ap_cfg.ap.ssid, s_hostname, sizeof(s_ap_cfg.ap.ssid));
    s_ap_cfg.ap.ssid_len = strlen(s_hostname);
    strlcpy((char *)s_ap_cfg.ap.password, BB_AP_PASSWORD, sizeof(s_ap_cfg.ap.password));
    s_ap_cfg.ap.channel = 1;
    s_ap_cfg.ap.max_connection = 4;
    s_ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
}

static void fill_sta_config(const bb_config_t *cfg)
{
    memset(&s_sta_cfg, 0, sizeof(s_sta_cfg));
    size_t ssid_len = strnlen(cfg->wifi_ssid, 32);
    memcpy(s_sta_cfg.sta.ssid, cfg->wifi_ssid, ssid_len);
    size_t pass_len = strnlen(cfg->wifi_password, 63);
    memcpy(s_sta_cfg.sta.password, cfg->wifi_password, pass_len);
    s_sta_cfg.sta.threshold.authmode =
        cfg->wifi_password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    s_sta_cfg.sta.pmf_cfg.capable = true;
    s_sta_cfg.sta.pmf_cfg.required = false;
}

static void set_ap_address(void)
{
    esp_netif_ip_info_t ip = {0};
    esp_netif_set_ip4_addr(&ip.ip, 192, 168, 4, 1);
    esp_netif_set_ip4_addr(&ip.gw, 192, 168, 4, 1);
    esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "Stopping AP DHCP server: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(s_ap_netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP address not set: %s", esp_err_to_name(err));
        return;
    }
    /* Stopping the server before the interface is up leaves it stopped.
     * Starting it now returns it to INIT, and the access-point start then
     * offers addresses from 192.168.4.1. */
    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "AP DHCP server not started: %s", esp_err_to_name(err));
    }
}

static void start_ap(void)
{
    wifi_mode_t mode = s_have_ssid ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Starting setup AP failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP config failed: %s", esp_err_to_name(err));
        return;
    }
    s_ap_active = true;
    set_ap_address();
}

static void stop_ap(void)
{
    if (!s_ap_active) {
        return;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Stopping setup AP failed: %s", esp_err_to_name(err));
        return;
    }
    s_ap_active = false;
    ESP_LOGI(TAG, "Setup AP stopped after 60 s on the station");
}

static void attempt_station(void)
{
    s_auto_reconnect = true;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BB_WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(BB_WIFI_ATTEMPT_MS));
    s_auto_reconnect = false;
    if ((bits & BB_WIFI_CONNECTED_BIT) != 0) {
        s_next_attempt_us = 0;
        return;
    }
    ESP_LOGW(TAG, "Station join failed for \"%s\"", (const char *)s_sta_cfg.sta.ssid);
    s_next_attempt_us = esp_timer_get_time() + BB_WIFI_RETRY_US;
}

static void wifi_task(void *arg)
{
    (void)arg;
    for (;;) {
        wifi_lock();
        const bool connected = s_have_ssid && bb_wifi_sta_connected();
        if (!connected) {
            s_connected_since_us = 0;
            if (!s_ap_active) {
                start_ap();
            }
            if (!s_have_ssid) {
                wifi_unlock();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            s_auto_reconnect = false;
            if (esp_timer_get_time() < s_next_attempt_us) {
                wifi_unlock();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "Retrying station");
            attempt_station();
            wifi_unlock();
            continue;
        }

        s_auto_reconnect = false;
        s_next_attempt_us = 0;
        if (s_connected_since_us == 0) {
            s_connected_since_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Station joined; setup AP stops in 60 s if the join holds");
        }
        if (s_ap_active && esp_timer_get_time() - s_connected_since_us >= BB_AP_HOLD_US) {
            stop_ap();
        }
        wifi_unlock();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void format_hostname(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_hostname, sizeof(s_hostname), "bambutton-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static bool ipv4_from_text(const char *text, esp_ip4_addr_t *out)
{
    return text != NULL && text[0] != '\0' && esp_netif_str_to_ip4(text, out) == ESP_OK;
}

static void apply_sta_address(const bb_config_t *cfg)
{
    if (cfg->use_dhcp) {
        esp_err_t err = esp_netif_dhcpc_start(s_sta_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "Starting DHCP client: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Station address from DHCP");
        return;
    }

    esp_netif_ip_info_t info = {0};
    if (!ipv4_from_text(cfg->ip, &info.ip) || !ipv4_from_text(cfg->netmask, &info.netmask) ||
        !ipv4_from_text(cfg->gateway, &info.gw)) {
        ESP_LOGW(TAG, "Static address is incomplete; using DHCP");
        return;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "Stopping DHCP client: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(s_sta_netif, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Static address not set: %s", esp_err_to_name(err));
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }

    const char *dns_text = cfg->dns[0] != '\0' ? cfg->dns : cfg->gateway;
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    if (ipv4_from_text(dns_text, &dns.ip.u_addr.ip4)) {
        err = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DNS not set: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "Static station address %s", cfg->ip);
}

esp_err_t bb_wifi_start(const bb_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    format_hostname();
    s_have_ssid = cfg->wifi_ssid[0] != '\0';
    fill_sta_config(cfg);
    fill_ap_config();
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, s_hostname));
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_ap_netif, s_hostname));
    set_ap_address();
    apply_sta_address(cfg);

    quiet_wifi_logs();
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event,
                                                        NULL, NULL));

    if (!s_have_ssid) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_err_t country = esp_wifi_set_country_code("DE", false);
        if (country != ESP_OK) {
            ESP_LOGW(TAG, "Country code: %s", esp_err_to_name(country));
        }
        s_ap_active = true;
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_err_t country = esp_wifi_set_country_code("DE", false);
        if (country != ESP_OK) {
            ESP_LOGW(TAG, "Country code: %s", esp_err_to_name(country));
        }
        ESP_LOGI(TAG, "Joining \"%s\" as %s", cfg->wifi_ssid, s_hostname);
        s_ap_active = true;
        attempt_station();
    }

    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_err = mdns_hostname_set(s_hostname);
    }
    if (mdns_err == ESP_OK) {
        mdns_err = mdns_instance_name_set(s_hostname);
    }
    if (mdns_err == ESP_OK) {
        mdns_err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS not started: %s", esp_err_to_name(mdns_err));
    } else {
        ESP_LOGI(TAG, "mDNS hostname %s.local", s_hostname);
    }

    if (xTaskCreate(wifi_task, "bb_wifi", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool bb_wifi_sta_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & BB_WIFI_CONNECTED_BIT) != 0;
}

bool bb_wifi_ap_active(void)
{
    return s_ap_active;
}

void bb_wifi_get_ip(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (bb_wifi_sta_connected() && s_sta_ip[0] != '\0') {
        snprintf(out, out_len, "%s", s_sta_ip);
        return;
    }
    if (s_ap_active) {
        snprintf(out, out_len, "192.168.4.1");
        return;
    }
    out[0] = '\0';
}

void bb_wifi_get_station(char *ssid, size_t ssid_len, char *ip, size_t ip_len, char *netmask, size_t netmask_len)
{
    if (ssid != NULL && ssid_len > 0) {
        ssid[0] = '\0';
    }
    if (ip != NULL && ip_len > 0) {
        ip[0] = '\0';
    }
    if (netmask != NULL && netmask_len > 0) {
        netmask[0] = '\0';
    }
    if (!bb_wifi_sta_connected()) {
        return;
    }
    wifi_lock();
    if (ssid != NULL && ssid_len > 0) {
        size_t n = ssid_len - 1;
        if (n > sizeof(s_sta_cfg.sta.ssid)) {
            n = sizeof(s_sta_cfg.sta.ssid);
        }
        memcpy(ssid, s_sta_cfg.sta.ssid, n);
        ssid[n] = '\0';
    }
    if (ip != NULL && ip_len > 0) {
        snprintf(ip, ip_len, "%s", s_sta_ip);
    }
    if (netmask != NULL && netmask_len > 0) {
        snprintf(netmask, netmask_len, "%s", s_sta_netmask);
    }
    wifi_unlock();
}

bool bb_wifi_format_signal(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (!bb_wifi_sta_connected()) {
        return false;
    }
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    const char *quality = "weak";
    if (ap.rssi >= -50) {
        quality = "excellent";
    } else if (ap.rssi >= -60) {
        quality = "good";
    } else if (ap.rssi >= -70) {
        quality = "fair";
    }
    snprintf(out, out_len, "%d dBm, %s", ap.rssi, quality);
    return true;
}

esp_err_t bb_wifi_apply_station(const bb_config_t *cfg, bool *joined)
{
    if (joined != NULL) {
        *joined = false;
    }
    if (cfg == NULL || cfg->wifi_ssid[0] == '\0' || s_events == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_lock();
    s_have_ssid = true;
    fill_sta_config(cfg);
    s_sta_ip[0] = '\0';
    s_sta_netmask[0] = '\0';
    s_connected_since_us = 0;
    xEventGroupClearBits(s_events, BB_WIFI_CONNECTED_BIT);

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK && mode != WIFI_MODE_APSTA) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) {
            s_ap_active = true;
            set_ap_address();
        }
    }
    if (err != ESP_OK) {
        wifi_unlock();
        return err;
    }

    apply_sta_address(cfg);
    xEventGroupClearBits(s_events, BB_WIFI_DROPPED_BIT);
    s_dropping = true;
    esp_err_t disc = esp_wifi_disconnect();
    if (disc == ESP_OK) {
        xEventGroupWaitBits(s_events, BB_WIFI_DROPPED_BIT, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));
    }
    s_dropping = false;
    err = esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
    if (err != ESP_OK) {
        wifi_unlock();
        return err;
    }
    ESP_LOGI(TAG, "Applying station \"%s\"", cfg->wifi_ssid);
    attempt_station();
    if (joined != NULL) {
        *joined = bb_wifi_sta_connected();
    }
    wifi_unlock();
    return ESP_OK;
}

const char *bb_wifi_hostname(void)
{
    return s_hostname;
}
