#include "bb_api.h"
#include "bb_button.h"
#include "bb_config.h"
#include "bb_led.h"
#include "bb_nvs.h"
#include "bb_ota.h"
#include "bb_web.h"
#include "bb_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb";

static bb_config_t s_cfg;
static bool s_awaiting;
static bool s_pending;
static bool s_needs_reconcile;
static bool s_chamber = true;
static int64_t s_retry_at;
static int64_t s_next_poll;
static unsigned s_clear_attempt;

/* Gap after a clear that failed or was not confirmed by printer status. */
#define BB_CLEAR_RETRY_US (2LL * 1000 * 1000)

static void runtime_status(bb_runtime_status_t *out)
{
    out->awaiting_plate_clear = s_awaiting;
    out->pending_press = s_pending;
    out->chamber_light = s_chamber;
}

static void apply_status(const bb_printer_status_t *status)
{
    s_awaiting = status->awaiting_plate_clear;
    s_chamber = status->chamber_light;
    if (s_pending) {
        if (s_awaiting) {
            s_needs_reconcile = false;
        } else {
            s_pending = false;
            s_needs_reconcile = false;
            s_retry_at = 0;
        }
    }
}

static void schedule_retry(void)
{
    s_retry_at = esp_timer_get_time() + BB_CLEAR_RETRY_US;
}

static void handle_pending(const bb_config_t *cfg)
{
    char err[96];
    if (s_needs_reconcile) {
        bb_printer_status_t status = {0};
        if (bb_api_get_status(cfg, &status, err, sizeof(err)) != ESP_OK) {
            ESP_LOGW(TAG, "Plate clear reconcile failed: %s", err);
            schedule_retry();
            return;
        }
        apply_status(&status);
        s_needs_reconcile = false;
        if (!s_pending) {
            s_clear_attempt = 0;
            return;
        }
    }

    s_clear_attempt++;
    if (bb_api_clear_plate(cfg, err, sizeof(err)) == ESP_OK) {
        bb_printer_status_t status = {0};
        if (bb_api_get_status(cfg, &status, err, sizeof(err)) == ESP_OK) {
            apply_status(&status);
            if (!s_pending) {
                ESP_LOGI(TAG, "Plate clear confirmed on attempt %u", s_clear_attempt);
                s_clear_attempt = 0;
                s_next_poll = esp_timer_get_time();
                return;
            }
            ESP_LOGW(TAG, "Plate clear attempt %u left the printer awaiting clear", s_clear_attempt);
            schedule_retry();
            return;
        }
        ESP_LOGW(TAG, "Plate clear attempt %u was accepted, status unread: %s", s_clear_attempt, err);
        s_needs_reconcile = true;
        schedule_retry();
        return;
    }

    ESP_LOGW(TAG, "Plate clear attempt %u failed: %s", s_clear_attempt, err);
    bb_printer_status_t status = {0};
    if (bb_api_get_status(cfg, &status, err, sizeof(err)) == ESP_OK) {
        apply_status(&status);
        if (!s_pending) {
            ESP_LOGI(TAG, "Plate clear confirmed by printer status on attempt %u", s_clear_attempt);
            s_clear_attempt = 0;
            return;
        }
        schedule_retry();
        return;
    }
    s_needs_reconcile = true;
    schedule_retry();
}

static void handle_poll(const bb_config_t *cfg)
{
    char err[96];
    bb_printer_status_t status = {0};
    if (bb_api_get_status(cfg, &status, err, sizeof(err)) != ESP_OK) {
        ESP_LOGW(TAG, "Printer status failed: %s", err);
        return;
    }
    apply_status(&status);
    ESP_LOGI(TAG, "Awaiting plate clear: %s, chamber light: %s", s_awaiting ? "yes" : "no",
             s_chamber ? "yes" : "no");
}

static bool image_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void log_row(const char *label, const char *value)
{
    printf("  %-16s %s\n", label, value != NULL && value[0] != '\0' ? value : "not set");
}

static void log_config(const bb_config_t *cfg)
{
    char text[16];
    printf("\n-------- BamButton-IDF --------\n");
    snprintf(text, sizeof(text), "GPIO %u", cfg->led_gpio);
    log_row("LED", text);
    snprintf(text, sizeof(text), "GPIO %u", cfg->button_gpio);
    log_row("Button", text);
    log_row("Wi-Fi SSID", cfg->wifi_ssid);
    log_row("Wi-Fi password", cfg->wifi_password[0] != '\0' ? "set" : "not set");
    if (cfg->use_dhcp) {
        log_row("Station address", "DHCP");
    } else {
        log_row("Station address", cfg->ip);
        log_row("Netmask", cfg->netmask);
        log_row("Gateway", cfg->gateway);
        log_row("DNS", cfg->dns);
    }
    log_row("Endpoint", cfg->endpoint);
    log_row("API key", cfg->api_key[0] != '\0' ? "set" : "not set");
    log_row("Ignore SSL", cfg->ignore_ssl ? "yes" : "no");
    if (cfg->printer_id > 0) {
        snprintf(text, sizeof(text), "%d", (int)cfg->printer_id);
        log_row("Printer", text);
    } else {
        log_row("Printer", "not set");
    }
    printf("-------------------------------\n\n");
    fflush(stdout);
}

void app_main(void)
{
    esp_err_t wdt = esp_task_wdt_add(NULL);
    if (wdt != ESP_OK && wdt != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "Watchdog subscribe: %s", esp_err_to_name(wdt));
    }

    esp_err_t nvs = bb_nvs_init();
    if (nvs != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs));
        bb_config_set_defaults(&s_cfg);
    } else if (bb_nvs_load(&s_cfg) != ESP_OK) {
        bb_config_set_defaults(&s_cfg);
    }
    log_config(&s_cfg);

    bb_led_start(s_cfg.led_gpio);
    bb_button_start(s_cfg.button_gpio);
    if (bb_api_init() != ESP_OK) {
        ESP_LOGE(TAG, "API client lock failed");
    }

    esp_err_t wifi = bb_wifi_start(&s_cfg);
    esp_err_t web = ESP_FAIL;
    if (wifi == ESP_OK) {
        web = bb_web_start(&s_cfg, runtime_status);
    } else {
        ESP_LOGE(TAG, "Wi-Fi failed: %s", esp_err_to_name(wifi));
    }
    if (wifi == ESP_OK && web != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed: %s", esp_err_to_name(web));
    }
    if ((wifi != ESP_OK || web != ESP_OK) && image_pending_verify()) {
        ESP_LOGE(TAG, "New firmware did not start its services; rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }

    bool confirmed = false;
    while (true) {
        if (wdt == ESP_OK) {
            esp_task_wdt_reset();
        }
        bb_led_set(!bb_wifi_sta_connected(), s_awaiting || s_pending, s_chamber);

        static int64_t last_press_us;
        if (bb_button_take_press()) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_press_us >= 150 * 1000 && s_awaiting) {
                s_awaiting = false;
                s_pending = true;
                s_needs_reconcile = false;
                s_clear_attempt = 0;
                s_retry_at = 0;
            }
            last_press_us = now_us;
        }

        bb_config_t cfg = s_cfg;
        if (web == ESP_OK) {
            bb_web_copy_config(&cfg);
        }
        const bool ready = bb_config_is_runtime_ready(&cfg);
        const bool online = bb_wifi_sta_connected() && !bb_ota_is_busy();
        const int64_t now = esp_timer_get_time();
        if (ready && online && s_pending && (s_retry_at == 0 || now >= s_retry_at)) {
            handle_pending(&cfg);
        }
        if (ready && online && !s_pending && now >= s_next_poll) {
            handle_poll(&cfg);
            s_next_poll = esp_timer_get_time() + 3LL * 1000 * 1000;
        }

        if (!confirmed && wifi == ESP_OK && web == ESP_OK) {
            esp_err_t mark = esp_ota_mark_app_valid_cancel_rollback();
            if (mark != ESP_OK && mark != ESP_ERR_NOT_FOUND && mark != ESP_FAIL) {
                ESP_LOGW(TAG, "Boot confirm: %s", esp_err_to_name(mark));
            }
            confirmed = true;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
