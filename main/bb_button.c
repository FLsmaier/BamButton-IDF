#include "bb_button.h"

#include "bb_config.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "bb.button";

static volatile bool s_pressed;

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    s_pressed = true;
}

void bb_button_start(int gpio)
{
    if (!bb_config_gpio_allowed((uint8_t)gpio)) {
        ESP_LOGE(TAG, "Button GPIO %d is not usable; using GPIO %d", gpio, BB_BUTTON_GPIO_DEFAULT);
        gpio = BB_BUTTON_GPIO_DEFAULT;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(gpio, button_isr, NULL));
    ESP_LOGI(TAG, "Button on GPIO %d", gpio);
}

bool bb_button_take_press(void)
{
    if (!s_pressed) {
        return false;
    }
    s_pressed = false;
    return true;
}

