#include "bb_led.h"

#include "bb_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bb.led";

static volatile bool s_link_down = true;
static volatile bool s_plate_alert = false;
static volatile bool s_idle_on = true;
static bool s_level = false;
static bool s_slow_phase = false;
static int s_gpio = BB_LED_GPIO_DEFAULT;

static void led_tick(void *arg)
{
    (void)arg;
    if (s_link_down) {
        s_slow_phase = false;
        s_level = !s_level;
        gpio_set_level(s_gpio, s_level ? 1 : 0);
        return;
    }
    if (s_plate_alert) {
        if (s_slow_phase) {
            s_level = !s_level;
            gpio_set_level(s_gpio, s_level ? 1 : 0);
        }
        s_slow_phase = !s_slow_phase;
        return;
    }
    s_slow_phase = false;
    gpio_set_level(s_gpio, s_idle_on ? 1 : 0);
}

void bb_led_set(bool link_down, bool plate_alert, bool idle_on)
{
    s_link_down = link_down;
    s_plate_alert = plate_alert;
    s_idle_on = idle_on;
}

void bb_led_start(int gpio)
{
    if (!bb_config_gpio_allowed((uint8_t)gpio)) {
        ESP_LOGE(TAG, "LED GPIO %d is not usable; using GPIO %d", gpio, BB_LED_GPIO_DEFAULT);
        gpio = BB_LED_GPIO_DEFAULT;
    }
    s_gpio = gpio;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(s_gpio, 0);

    const esp_timer_create_args_t args = {
        .callback = led_tick,
        .name = "bb_led",
    };
    esp_timer_handle_t timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    /* 125 ms toggles the connection alarm. Plate-clear toggles every other tick. */
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 125 * 1000));
    ESP_LOGI(TAG, "LED on GPIO %d", s_gpio);
}
