#pragma once

#include "bb_config.h"

#include "esp_err.h"

esp_err_t bb_nvs_init(void);
esp_err_t bb_nvs_load(bb_config_t *cfg);
esp_err_t bb_nvs_save(const bb_config_t *cfg);
esp_err_t bb_nvs_erase(void);
