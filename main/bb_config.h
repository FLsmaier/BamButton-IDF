#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BB_CONFIG_VERSION 4

/* Bambutton defaults: LED on GPIO3, button on GPIO4. */
#define BB_LED_GPIO_DEFAULT 3
#define BB_BUTTON_GPIO_DEFAULT 4

/*
 * Header pins on the ESP32-C3 Super Mini that can be a button or an LED.
 * GPIO2, GPIO8, and GPIO9 are strapping pins: GPIO9 is the BOOT button
 * (low at reset enters download mode), GPIO8 must stay high to enter
 * download mode and drives the onboard LED, and Espressif recommends GPIO2
 * high at reset. GPIO11-17 are the in-package flash and GPIO18-19 are USB,
 * and none of those are on the header. GPIO20 and GPIO21 are UART0; this
 * firmware's console is USB, so they are usable after boot.
 */
#define BB_GPIO_CHOICE_COUNT 10

#define BB_SSID_CAP 33
#define BB_PASSWORD_CAP 65
#define BB_ENDPOINT_CAP 160
#define BB_API_KEY_CAP 128
#define BB_URL_CAP 192
#define BB_IPV4_CAP 16

/*
 * wifi_low_tx and the pad keep printer_id 4-byte aligned. led_gpio and
 * button_gpio occupy the two bytes that version 2 left reserved, so a
 * version 2 blob is the same size and is loaded with the default pins.
 * wifi_low_tx reuses a pad byte, so versions 2 and 3 stay the same size
 * and load with the 8.5 dBm cap on. A blob is rejected when its size does
 * not match, or when its version is not 2, 3, or 4.
 */
typedef struct {
    uint32_t version;
    char wifi_ssid[BB_SSID_CAP];
    char wifi_password[BB_PASSWORD_CAP];
    char endpoint[BB_ENDPOINT_CAP];
    char api_key[BB_API_KEY_CAP];
    char ip[BB_IPV4_CAP];
    char netmask[BB_IPV4_CAP];
    char gateway[BB_IPV4_CAP];
    char dns[BB_IPV4_CAP];
    uint8_t wifi_low_tx;
    uint8_t pad_align;
    int32_t printer_id;
    uint8_t ignore_ssl;
    uint8_t use_dhcp;
    uint8_t led_gpio;
    uint8_t button_gpio;
} bb_config_t;

void bb_config_set_defaults(bb_config_t *cfg);

uint8_t bb_config_gpio_choice(size_t index);
bool bb_config_gpio_allowed(uint8_t gpio);
bool bb_config_pins_valid(const bb_config_t *cfg);

/* Trimmed endpoint to an API root with no trailing slash. */
bool bb_config_format_base_url(const char *endpoint, char *out, size_t out_len);

bool bb_config_join_url(const char *base, const char *path, char *out, size_t out_len);

bool bb_config_is_runtime_ready(const bb_config_t *cfg);

/*
 * password_set / api_key_set are false when the form left the secret blank,
 * which keeps the stored value. Empty SSID and empty endpoint are stored.
 */
bool bb_config_update(bb_config_t *cfg,
                      const char *ssid,
                      const char *password,
                      bool password_set,
                      const char *endpoint,
                      const char *api_key,
                      bool api_key_set,
                      bool ignore_ssl,
                      bool use_dhcp,
                      bool wifi_low_tx,
                      const char *ip,
                      const char *netmask,
                      const char *gateway,
                      const char *dns,
                      int32_t printer_id,
                      uint8_t led_gpio,
                      uint8_t button_gpio,
                      char *err,
                      size_t err_len);
