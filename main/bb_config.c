#include "bb_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(bb_config_t) == 464, "bb_config_t NVS layout changed");
_Static_assert(offsetof(bb_config_t, printer_id) % 4 == 0, "printer_id alignment");
_Static_assert(offsetof(bb_config_t, led_gpio) == sizeof(bb_config_t) - 2, "pin bytes moved");

static const uint8_t k_gpio_choices[BB_GPIO_CHOICE_COUNT] = {0, 1, 3, 4, 5, 6, 7, 10, 20, 21};

static bool copy_str(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return false;
    }
    if (src == NULL) {
        src = "";
    }
    size_t len = strlen(src);
    if (len >= dst_len) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

static bool trim_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return false;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return true;
    }
    while (*src != '\0' && isspace((unsigned char)*src)) {
        src++;
    }
    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        len--;
    }
    if (len >= dst_len) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    if (msg == NULL) {
        err[0] = '\0';
        return;
    }
    size_t len = strlen(msg);
    if (len >= err_len) {
        len = err_len - 1;
    }
    memcpy(err, msg, len);
    err[len] = '\0';
}

static void set_labeled(char *err, size_t err_len, const char *label, const char *detail)
{
    char msg[80];
    int wrote = snprintf(msg, sizeof(msg), "%s %s", label != NULL ? label : "Value", detail);
    if (wrote < 0 || (size_t)wrote >= sizeof(msg)) {
        set_err(err, err_len, detail);
        return;
    }
    set_err(err, err_len, msg);
}

static bool all_digits(const char *s)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static void strip_trailing_slashes(char *path)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

void bb_config_set_defaults(bb_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = BB_CONFIG_VERSION;
    cfg->use_dhcp = 1;
    cfg->led_gpio = BB_LED_GPIO_DEFAULT;
    cfg->button_gpio = BB_BUTTON_GPIO_DEFAULT;
}

uint8_t bb_config_gpio_choice(size_t index)
{
    if (index >= BB_GPIO_CHOICE_COUNT) {
        return BB_LED_GPIO_DEFAULT;
    }
    return k_gpio_choices[index];
}

bool bb_config_gpio_allowed(uint8_t gpio)
{
    for (size_t i = 0; i < BB_GPIO_CHOICE_COUNT; i++) {
        if (k_gpio_choices[i] == gpio) {
            return true;
        }
    }
    return false;
}

bool bb_config_pins_valid(const bb_config_t *cfg)
{
    return cfg != NULL && bb_config_gpio_allowed(cfg->led_gpio) &&
           bb_config_gpio_allowed(cfg->button_gpio) && cfg->led_gpio != cfg->button_gpio;
}

bool bb_config_format_base_url(const char *endpoint, char *out, size_t out_len)
{
    char raw[BB_ENDPOINT_CAP];
    char authority[BB_ENDPOINT_CAP];
    char path[BB_ENDPOINT_CAP];
    char host[BB_ENDPOINT_CAP];

    if (out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (!trim_copy(raw, sizeof(raw), endpoint) || raw[0] == '\0') {
        return false;
    }

    const char *scheme = NULL;
    const char *rest = raw;
    if (strncmp(raw, "https://", 8) == 0) {
        scheme = "https";
        rest = raw + 8;
    } else if (strncmp(raw, "http://", 7) == 0) {
        scheme = "http";
        rest = raw + 7;
    }
    if (rest[0] == '\0' || rest[0] == '/') {
        return false;
    }

    const char *slash = strchr(rest, '/');
    if (slash != NULL) {
        size_t auth_len = (size_t)(slash - rest);
        if (auth_len == 0 || auth_len >= sizeof(authority)) {
            return false;
        }
        memcpy(authority, rest, auth_len);
        authority[auth_len] = '\0';
        if (!copy_str(path, sizeof(path), slash)) {
            return false;
        }
    } else {
        if (!copy_str(authority, sizeof(authority), rest)) {
            return false;
        }
        path[0] = '\0';
    }

    int port = -1;
    const char *colon = strrchr(authority, ':');
    if (colon != NULL && colon != authority && all_digits(colon + 1)) {
        port = 0;
        for (const char *p = colon + 1; *p != '\0'; p++) {
            port = port * 10 + (*p - '0');
            if (port > 65535) {
                return false;
            }
        }
        if (port == 0) {
            return false;
        }
        size_t host_len = (size_t)(colon - authority);
        if (host_len == 0 || host_len >= sizeof(host)) {
            return false;
        }
        memcpy(host, authority, host_len);
        host[host_len] = '\0';
    } else if (!copy_str(host, sizeof(host), authority)) {
        return false;
    }
    if (host[0] == '\0' || strchr(host, ' ') != NULL) {
        return false;
    }

    if (scheme == NULL) {
        scheme = (port == 80) ? "http" : "https";
    }

    strip_trailing_slashes(path);
    if (path[0] == '\0' || strcmp(path, "/") == 0) {
        if (!copy_str(path, sizeof(path), "/api/v1")) {
            return false;
        }
    }

    const bool omit_port =
        (strcmp(scheme, "https") == 0 && (port < 0 || port == 443)) ||
        (strcmp(scheme, "http") == 0 && (port < 0 || port == 80));

    int wrote;
    if (omit_port) {
        wrote = snprintf(out, out_len, "%s://%s%s", scheme, host, path);
    } else {
        wrote = snprintf(out, out_len, "%s://%s:%d%s", scheme, host, port, path);
    }
    return wrote > 0 && (size_t)wrote < out_len;
}

bool bb_config_join_url(const char *base, const char *path, char *out, size_t out_len)
{
    if (base == NULL || path == NULL || out == NULL || out_len == 0) {
        return false;
    }
    while (*path == '/') {
        path++;
    }
    int wrote = snprintf(out, out_len, "%s/%s", base, path);
    return wrote > 0 && (size_t)wrote < out_len;
}

bool bb_config_is_runtime_ready(const bb_config_t *cfg)
{
    char base[BB_URL_CAP];
    if (cfg == NULL) {
        return false;
    }
    return cfg->wifi_ssid[0] != '\0' && cfg->endpoint[0] != '\0' &&
           cfg->api_key[0] != '\0' && cfg->printer_id >= 1 &&
           bb_config_format_base_url(cfg->endpoint, base, sizeof(base));
}

static bool parse_ipv4(const char *text,
                       char *out,
                       size_t out_len,
                       bool required,
                       const char *label,
                       char *err,
                       size_t err_len)
{
    char trimmed[BB_IPV4_CAP];
    if (!trim_copy(trimmed, sizeof(trimmed), text)) {
        set_labeled(err, err_len, label, "is not an IPv4 address");
        return false;
    }
    if (trimmed[0] == '\0') {
        if (required) {
            set_labeled(err, err_len, label, "is required");
            return false;
        }
        if (out != NULL && out_len > 0) {
            out[0] = '\0';
        }
        return true;
    }
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;
    char extra = 0;
    if (sscanf(trimmed, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 ||
        c > 255 || d > 255 || (a == 0 && b == 0 && c == 0 && d == 0)) {
        set_labeled(err, err_len, label, "is not an IPv4 address");
        return false;
    }
    int wrote = snprintf(out, out_len, "%u.%u.%u.%u", a, b, c, d);
    if (wrote < 0 || out == NULL || (size_t)wrote >= out_len) {
        if (out != NULL && out_len > 0) {
            out[0] = '\0';
        }
        set_labeled(err, err_len, label, "is not an IPv4 address");
        return false;
    }
    return true;
}

bool bb_config_update(bb_config_t *cfg,
                      const char *ssid,
                      const char *password,
                      bool password_set,
                      const char *endpoint,
                      const char *api_key,
                      bool api_key_set,
                      bool ignore_ssl,
                      bool use_dhcp,
                      const char *ip,
                      const char *netmask,
                      const char *gateway,
                      const char *dns,
                      int32_t printer_id,
                      uint8_t led_gpio,
                      uint8_t button_gpio,
                      char *err,
                      size_t err_len)
{
    char trimmed_ssid[BB_SSID_CAP];
    char trimmed_endpoint[BB_ENDPOINT_CAP];
    char base[BB_URL_CAP];

    if (cfg == NULL) {
        set_err(err, err_len, "Missing configuration");
        return false;
    }

    if (!trim_copy(trimmed_ssid, sizeof(trimmed_ssid), ssid)) {
        set_err(err, err_len, "Wi-Fi SSID is too long");
        return false;
    }
    if (ssid != NULL) {
        const char *start = ssid;
        while (*start != '\0' && isspace((unsigned char)*start)) {
            start++;
        }
        size_t len = strlen(start);
        while (len > 0 && isspace((unsigned char)start[len - 1])) {
            len--;
        }
        if (len > 32) {
            set_err(err, err_len, "Wi-Fi SSID is too long");
            return false;
        }
    }

    if (password_set) {
        if (password == NULL) {
            password = "";
        }
        size_t len = strlen(password);
        if (len > 0 && len < 8) {
            set_err(err, err_len, "Wi-Fi password must be empty or at least 8 characters");
            return false;
        }
        if (len > 63) {
            set_err(err, err_len, "Wi-Fi password is too long");
            return false;
        }
    }

    if (!trim_copy(trimmed_endpoint, sizeof(trimmed_endpoint), endpoint)) {
        set_err(err, err_len, "Endpoint is too long");
        return false;
    }
    if (endpoint != NULL) {
        const char *start = endpoint;
        while (*start != '\0' && isspace((unsigned char)*start)) {
            start++;
        }
        size_t len = strlen(start);
        while (len > 0 && isspace((unsigned char)start[len - 1])) {
            len--;
        }
        if (len >= BB_ENDPOINT_CAP) {
            set_err(err, err_len, "Endpoint is too long");
            return false;
        }
    }
    if (trimmed_endpoint[0] != '\0' &&
        !bb_config_format_base_url(trimmed_endpoint, base, sizeof(base))) {
        set_err(err, err_len, "Endpoint is not a valid host or URL");
        return false;
    }

    if (api_key_set) {
        if (api_key == NULL) {
            api_key = "";
        }
        size_t len = strlen(api_key);
        if (len >= BB_API_KEY_CAP) {
            set_err(err, err_len, "API key is too long");
            return false;
        }
    }

    if (printer_id < 0) {
        set_err(err, err_len, "Printer must be a positive number");
        return false;
    }
    if (!bb_config_gpio_allowed(led_gpio)) {
        set_err(err, err_len, "LED GPIO is not available on this board");
        return false;
    }
    if (!bb_config_gpio_allowed(button_gpio)) {
        set_err(err, err_len, "Button GPIO is not available on this board");
        return false;
    }
    if (led_gpio == button_gpio) {
        set_err(err, err_len, "LED and button must use different GPIOs");
        return false;
    }

    char ip_text[BB_IPV4_CAP];
    char mask_text[BB_IPV4_CAP];
    char gateway_text[BB_IPV4_CAP];
    char dns_text[BB_IPV4_CAP];
    if (!parse_ipv4(ip, ip_text, sizeof(ip_text), !use_dhcp, "IP address", err, err_len) ||
        !parse_ipv4(netmask, mask_text, sizeof(mask_text), !use_dhcp, "Netmask", err, err_len) ||
        !parse_ipv4(gateway, gateway_text, sizeof(gateway_text), !use_dhcp, "Gateway", err, err_len) ||
        !parse_ipv4(dns, dns_text, sizeof(dns_text), false, "DNS", err, err_len)) {
        return false;
    }
    if (!use_dhcp && dns_text[0] == '\0' && !copy_str(dns_text, sizeof(dns_text), gateway_text)) {
        set_err(err, err_len, "DNS is not an IPv4 address");
        return false;
    }

    if (!copy_str(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), trimmed_ssid) ||
        (password_set && !copy_str(cfg->wifi_password, sizeof(cfg->wifi_password), password)) ||
        !copy_str(cfg->endpoint, sizeof(cfg->endpoint), trimmed_endpoint) ||
        (api_key_set && !copy_str(cfg->api_key, sizeof(cfg->api_key), api_key))) {
        set_err(err, err_len, "A setting does not fit in storage");
        return false;
    }
    cfg->ignore_ssl = ignore_ssl ? 1 : 0;
    cfg->use_dhcp = use_dhcp ? 1 : 0;
    if (!copy_str(cfg->ip, sizeof(cfg->ip), ip_text) ||
        !copy_str(cfg->netmask, sizeof(cfg->netmask), mask_text) ||
        !copy_str(cfg->gateway, sizeof(cfg->gateway), gateway_text) ||
        !copy_str(cfg->dns, sizeof(cfg->dns), dns_text)) {
        set_err(err, err_len, "A setting does not fit in storage");
        return false;
    }
    cfg->printer_id = printer_id;
    cfg->led_gpio = led_gpio;
    cfg->button_gpio = button_gpio;
    cfg->version = BB_CONFIG_VERSION;
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    return true;
}
