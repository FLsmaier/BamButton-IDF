#pragma once

#include "bb_config.h"

#include "esp_err.h"

typedef struct {
    bool awaiting_plate_clear;
    bool chamber_light;
} bb_printer_status_t;

esp_err_t bb_api_init(void);
esp_err_t bb_api_get_printers(const bb_config_t *cfg, char *body, size_t body_len, char *err, size_t err_len);
esp_err_t bb_api_get_status(const bb_config_t *cfg, bb_printer_status_t *status, char *err, size_t err_len);
esp_err_t bb_api_clear_plate(const bb_config_t *cfg, char *err, size_t err_len);
