#pragma once

#include "bb_config.h"

#include "esp_err.h"

typedef struct {
    bool awaiting_plate_clear;
    bool pending_press;
    bool chamber_light;
} bb_runtime_status_t;

typedef void (*bb_runtime_fn)(bb_runtime_status_t *out);

esp_err_t bb_web_start(bb_config_t *cfg, bb_runtime_fn runtime);
void bb_web_copy_config(bb_config_t *out);
