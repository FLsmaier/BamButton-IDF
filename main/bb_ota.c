#include "bb_ota.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb.ota";
#define BB_OTA_CHUNK 1024

static atomic_bool s_busy = false;
static char s_chunk[BB_OTA_CHUNK];

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", msg != NULL ? msg : "");
    }
}

bool bb_ota_begin_session(void)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&s_busy, &expected, true);
}

void bb_ota_end_session(void)
{
    atomic_store(&s_busy, false);
}

bool bb_ota_is_busy(void)
{
    return atomic_load(&s_busy);
}

static bool is_semver(const char *version)
{
    int dots = 0;
    if (version == NULL || *version == '\0') {
        return false;
    }
    for (const char *p = version; *p != '\0' && *p != '-'; p++) {
        if (*p == '.') {
            dots++;
        } else if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return dots == 2;
}

static int compare_versions(const char *v1, const char *v2)
{
    while (*v1 != '\0' || *v2 != '\0') {
        int n1 = 0;
        int n2 = 0;
        while (*v1 != '\0' && *v1 != '.' && *v1 != '-') {
            if (!isdigit((unsigned char)*v1)) {
                return 0;
            }
            n1 = n1 * 10 + (*v1 - '0');
            v1++;
        }
        while (*v2 != '\0' && *v2 != '.' && *v2 != '-') {
            if (!isdigit((unsigned char)*v2)) {
                return 0;
            }
            n2 = n2 * 10 + (*v2 - '0');
            v2++;
        }
        if (n1 != n2) {
            return (n1 > n2) ? 1 : -1;
        }
        if (*v1 == '.' || *v1 == '-') {
            v1++;
        }
        if (*v2 == '.' || *v2 == '-') {
            v2++;
        }
    }
    return 0;
}

static bool is_dirty(const char *version)
{
    return strstr(version, "-dirty") != NULL || strstr(version, "DEBUG") != NULL;
}

static void version_text(const char version[32], char out[33])
{
    memcpy(out, version, 32);
    out[32] = '\0';
}

static esp_err_t check_version(const esp_app_desc_t *incoming, bool force, char *err, size_t err_len)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t current = {0};
    char new_version[33];
    char running_version[33];
    version_text(incoming->version, new_version);
    if (running == NULL || esp_ota_get_partition_description(running, &current) != ESP_OK) {
        set_err(err, err_len, "Running firmware description is unavailable");
        return ESP_FAIL;
    }
    version_text(current.version, running_version);
    ESP_LOGI(TAG, "Running %s, upload %s", running_version, new_version);

    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid != NULL) {
        esp_app_desc_t invalid_info = {0};
        if (esp_ota_get_partition_description(invalid, &invalid_info) == ESP_OK &&
            memcmp(invalid_info.version, incoming->version, sizeof(incoming->version)) == 0) {
            snprintf(err, err_len, "Version %s previously failed and was rolled back", new_version);
            return ESP_FAIL;
        }
    }

    if (force) {
        set_err(err, err_len, "");
        return ESP_OK;
    }
    if (memcmp(current.version, incoming->version, sizeof(current.version)) == 0) {
        snprintf(err, err_len, "Version %s is already running", running_version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (is_dirty(new_version)) {
        snprintf(err, err_len, "Version %s looks like a debug build. Check Force to install it", new_version);
        return ESP_ERR_INVALID_VERSION;
    }

    const bool new_semver = is_semver(new_version);
    const bool running_semver = is_semver(running_version);
    if (new_semver && running_semver && compare_versions(new_version, running_version) <= 0) {
        snprintf(err, err_len, "Version %s is not newer than %s", new_version, running_version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!new_semver && running_semver) {
        snprintf(err, err_len, "Version %s is not a newer semver than %s", new_version, running_version);
        return ESP_ERR_INVALID_VERSION;
    }
    set_err(err, err_len, "");
    return ESP_OK;
}

esp_err_t bb_ota_update_from_reader(const bb_ota_reader_t *reader, bool force, char *err, size_t err_len)
{
    if (reader == NULL || reader->read == NULL) {
        set_err(err, err_len, "Invalid firmware stream");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        set_err(err, err_len, "No inactive firmware slot");
        return ESP_FAIL;
    }

    bool began = false;
    esp_ota_handle_t handle = 0;
    bool checked = false;
    int total = 0;

    for (;;) {
        taskYIELD();
        int got = reader->read(reader->ctx, s_chunk, sizeof(s_chunk));
        if (got < 0) {
            set_err(err, err_len, "Firmware upload was interrupted");
            if (began) {
                esp_ota_abort(handle);
            }
            return ESP_FAIL;
        }
        if (got == 0) {
            break;
        }
        if (!checked) {
            const int need = (int)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                                   sizeof(esp_app_desc_t));
            if (got <= need || (uint8_t)s_chunk[0] != ESP_IMAGE_HEADER_MAGIC) {
                set_err(err, err_len, "File is not an ESP application image");
                return ESP_FAIL;
            }
            esp_app_desc_t incoming;
            memcpy(&incoming,
                   s_chunk + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
                   sizeof(incoming));
            if (incoming.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                set_err(err, err_len, "File is not an ESP application image");
                return ESP_FAIL;
            }
            esp_err_t gate = check_version(&incoming, force, err, err_len);
            if (gate != ESP_OK) {
                return gate;
            }
            esp_err_t begin = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (begin != ESP_OK) {
                snprintf(err, err_len, "Could not start the firmware write (%s)", esp_err_to_name(begin));
                return begin;
            }
            began = true;
            checked = true;
        }
        esp_err_t write = esp_ota_write(handle, s_chunk, (size_t)got);
        if (write != ESP_OK) {
            esp_ota_abort(handle);
            snprintf(err, err_len, "Firmware write failed (%s)", esp_err_to_name(write));
            return write;
        }
        total += got;
    }

    if (!checked) {
        set_err(err, err_len, "Firmware file was empty");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Wrote %d bytes to %s", total, update->label);
    esp_err_t end = esp_ota_end(handle);
    if (end != ESP_OK) {
        snprintf(err, err_len, "Firmware image was rejected (%s)", esp_err_to_name(end));
        return end;
    }
    esp_err_t boot = esp_ota_set_boot_partition(update);
    if (boot != ESP_OK) {
        snprintf(err, err_len, "Could not select the new firmware (%s)", esp_err_to_name(boot));
        return boot;
    }
    set_err(err, err_len, "Upgrade successful. Rebooting now.");
    return ESP_OK;
}
