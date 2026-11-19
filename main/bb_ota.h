#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define BB_OTA_ERR_CAP 192

bool bb_ota_begin_session(void);
void bb_ota_end_session(void);
bool bb_ota_is_busy(void);

typedef int (*bb_ota_read_fn)(void *ctx, char *buf, size_t max_len);

typedef struct {
    bb_ota_read_fn read;
    void *ctx;
} bb_ota_reader_t;

esp_err_t bb_ota_update_from_reader(const bb_ota_reader_t *reader, bool force, char *err, size_t err_len);
