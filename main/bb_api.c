#include "bb_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bb.api";
#define BB_API_BODY_CAP 8192
#define BB_API_TIMEOUT_MS 5000
#define BB_API_ATTEMPTS 3
#define BB_API_RETRY_MS 500

static SemaphoreHandle_t s_lock;

esp_err_t bb_api_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", msg != NULL ? msg : "");
    }
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} bb_body_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->user_data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    bb_body_t *body = evt->user_data;
    if (body->len + (size_t)evt->data_len + 1 > body->cap) {
        body->overflow = true;
        return ESP_OK;
    }
    memcpy(body->buf + body->len, evt->data, (size_t)evt->data_len);
    body->len += (size_t)evt->data_len;
    body->buf[body->len] = '\0';
    return ESP_OK;
}

static esp_err_t api_request(const bb_config_t *cfg,
                             const char *path,
                             bool post,
                             char *body,
                             size_t body_len,
                             int *status_out,
                             char *err,
                             size_t err_len)
{
    char base[BB_URL_CAP];
    char url[BB_URL_CAP + 64];
    if (cfg == NULL || path == NULL || body == NULL || body_len < 2) {
        set_err(err, err_len, "Invalid API request");
        return ESP_ERR_INVALID_ARG;
    }
    if (!bb_config_format_base_url(cfg->endpoint, base, sizeof(base)) ||
        !bb_config_join_url(base, path, url, sizeof(url))) {
        set_err(err, err_len, "Endpoint is not a valid host or URL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        set_err(err, err_len, "API client is busy");
        return ESP_ERR_TIMEOUT;
    }

    bb_body_t collected = {
        .buf = body,
        .cap = body_len,
        .len = 0,
        .overflow = false,
    };

    esp_http_client_config_t http = {
        .url = url,
        .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = BB_API_TIMEOUT_MS,
        .event_handler = http_event,
        .user_data = &collected,
        .buffer_size = 1024,
    };
    const bool https = strncmp(url, "https://", 8) == 0;
    if (https && cfg->ignore_ssl == 0) {
        http.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_err_t err_code = ESP_FAIL;
    int status = 0;
    for (int attempt = 1; attempt <= BB_API_ATTEMPTS; attempt++) {
        collected.len = 0;
        collected.overflow = false;
        body[0] = '\0';

        esp_http_client_handle_t client = esp_http_client_init(&http);
        if (client == NULL) {
            xSemaphoreGive(s_lock);
            set_err(err, err_len, "HTTP client init failed");
            return ESP_ERR_NO_MEM;
        }
        esp_http_client_set_header(client, "X-API-Key", cfg->api_key);
        esp_http_client_set_header(client, "Accept", "application/json");
        if (post) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, "", 0);
        }

        err_code = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        const bool transport_failed = err_code != ESP_OK || status >= 500;
        if (!transport_failed || collected.overflow || attempt == BB_API_ATTEMPTS) {
            break;
        }
        ESP_LOGW(TAG, "%s %s attempt %d/%d failed: %s", post ? "POST" : "GET", path, attempt, BB_API_ATTEMPTS,
                 err_code != ESP_OK ? esp_err_to_name(err_code) : "server error");
        vTaskDelay(pdMS_TO_TICKS(BB_API_RETRY_MS));
    }
    xSemaphoreGive(s_lock);

    if (status_out != NULL) {
        *status_out = status;
    }
    if (err_code != ESP_OK) {
        ESP_LOGW(TAG, "%s %s failed: %s", post ? "POST" : "GET", path, esp_err_to_name(err_code));
        set_err(err, err_len, "Bambuddy request failed");
        return err_code;
    }
    if (collected.overflow) {
        set_err(err, err_len, "Bambuddy response is too large");
        return ESP_ERR_NO_MEM;
    }
    if (status < 200 || status >= 300) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Bambuddy returned %d", status);
        set_err(err, err_len, msg);
        return ESP_FAIL;
    }
    set_err(err, err_len, "");
    return ESP_OK;
}

static bool json_bool(const cJSON *item, bool *out)
{
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble != 0;
        return true;
    }
    return false;
}

esp_err_t bb_api_get_printers(const bb_config_t *cfg, char *body, size_t body_len, char *err, size_t err_len)
{
    int status = 0;
    return api_request(cfg, "printers/", false, body, body_len, &status, err, err_len);
}

esp_err_t bb_api_get_status(const bb_config_t *cfg, bb_printer_status_t *status, char *err, size_t err_len)
{
    if (status == NULL || cfg == NULL) {
        set_err(err, err_len, "Invalid API request");
        return ESP_ERR_INVALID_ARG;
    }
    char path[40];
    snprintf(path, sizeof(path), "printers/%d/status", (int)cfg->printer_id);
    char *body = malloc(BB_API_BODY_CAP);
    if (body == NULL) {
        set_err(err, err_len, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err_code = api_request(cfg, path, false, body, BB_API_BODY_CAP, NULL, err, err_len);
    if (err_code != ESP_OK) {
        free(body);
        return err_code;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        set_err(err, err_len, "Bambuddy status was not JSON");
        return ESP_FAIL;
    }
    bool awaiting = false;
    bool chamber = false;
    bool ok = json_bool(cJSON_GetObjectItem(root, "awaiting_plate_clear"), &awaiting) &&
              json_bool(cJSON_GetObjectItem(root, "chamber_light"), &chamber);
    cJSON_Delete(root);
    if (!ok) {
        set_err(err, err_len, "Bambuddy status is missing plate or light fields");
        return ESP_FAIL;
    }
    status->awaiting_plate_clear = awaiting;
    status->chamber_light = chamber;
    set_err(err, err_len, "");
    return ESP_OK;
}

esp_err_t bb_api_clear_plate(const bb_config_t *cfg, char *err, size_t err_len)
{
    if (cfg == NULL) {
        set_err(err, err_len, "Invalid API request");
        return ESP_ERR_INVALID_ARG;
    }
    char path[48];
    snprintf(path, sizeof(path), "printers/%d/clear-plate", (int)cfg->printer_id);
    char *body = malloc(1024);
    if (body == NULL) {
        set_err(err, err_len, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err_code = api_request(cfg, path, true, body, 1024, NULL, err, err_len);
    free(body);
    return err_code;
}
