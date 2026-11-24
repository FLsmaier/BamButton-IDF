#include "bb_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "bb.nvs";
static const char *NVS_NAMESPACE = "bambutton";
static const char *NVS_KEY = "cfg";

static void force_terminated(bb_config_t *cfg)
{
    cfg->wifi_ssid[BB_SSID_CAP - 1] = '\0';
    cfg->wifi_password[BB_PASSWORD_CAP - 1] = '\0';
    cfg->endpoint[BB_ENDPOINT_CAP - 1] = '\0';
    cfg->api_key[BB_API_KEY_CAP - 1] = '\0';
    cfg->ip[BB_IPV4_CAP - 1] = '\0';
    cfg->netmask[BB_IPV4_CAP - 1] = '\0';
    cfg->gateway[BB_IPV4_CAP - 1] = '\0';
    cfg->dns[BB_IPV4_CAP - 1] = '\0';
}

esp_err_t bb_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing incompatible NVS partition");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t bb_nvs_load(bb_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bb_config_set_defaults(cfg);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved settings");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    bb_config_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    size_t size = sizeof(loaded);
    err = nvs_get_blob(handle, NVS_KEY, &loaded, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved settings");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        return err;
    }
    if (size != sizeof(loaded) ||
        (loaded.version != 2 && loaded.version != 3 && loaded.version != BB_CONFIG_VERSION)) {
        ESP_LOGW(TAG, "Settings blob ignored (size %u version %u)", (unsigned)size,
                 (unsigned)loaded.version);
        return ESP_OK;
    }

    force_terminated(&loaded);
    if (loaded.version == 2) {
        loaded.led_gpio = BB_LED_GPIO_DEFAULT;
        loaded.button_gpio = BB_BUTTON_GPIO_DEFAULT;
        ESP_LOGI(TAG, "Settings from version 2; LED GPIO %u and button GPIO %u", loaded.led_gpio,
                 loaded.button_gpio);
    } else if (!bb_config_pins_valid(&loaded)) {
        ESP_LOGW(TAG, "Stored pins GPIO %u and GPIO %u are not usable; using defaults",
                 loaded.led_gpio, loaded.button_gpio);
        loaded.led_gpio = BB_LED_GPIO_DEFAULT;
        loaded.button_gpio = BB_BUTTON_GPIO_DEFAULT;
    }
    if (loaded.version < 4) {
        loaded.wifi_low_tx = 1;
        ESP_LOGI(TAG, "Settings from version %u; Wi-Fi power capped at 8.5 dBm",
                 (unsigned)loaded.version);
    } else if (loaded.wifi_low_tx > 1) {
        loaded.wifi_low_tx = 1;
    }
    loaded.version = BB_CONFIG_VERSION;
    *cfg = loaded;
    ESP_LOGI(TAG, "Loaded settings for SSID \"%s\"", cfg->wifi_ssid);
    return ESP_OK;
}

esp_err_t bb_nvs_save(const bb_config_t *cfg)
{
    if (cfg == NULL || cfg->version != BB_CONFIG_VERSION) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving settings failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Saved settings");
    return ESP_OK;
}

esp_err_t bb_nvs_erase(void)
{
    ESP_LOGW(TAG, "Erasing NVS");
    return nvs_flash_erase();
}
