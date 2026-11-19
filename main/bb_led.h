#pragma once

#include <stdbool.h>

void bb_led_start(int gpio);
void bb_led_set(bool link_down, bool plate_alert, bool idle_on);
