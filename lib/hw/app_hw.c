#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "app_hw.h"

#define TAG "apphw"

#define RELAY_ON 1
#define RELAY_OFF 0
#define BTN_PRESSED 0   // internal pull-up: pin reads LOW when pressed
#define POLL_MS 20

// button -> relay -> fan speed value (OFF button has no relay)
static const btn_map_t btn_map[] = {
    {APP_BTN0, GPIO_NUM_NC, 0},
    {APP_BTN1, APP_RELAY1, 33},
    {APP_BTN2, APP_RELAY2, 66},
    {APP_BTN3, APP_RELAY3, 100},
};
#define BTN_COUNT (sizeof(btn_map) / sizeof(btn_map[0]))

static appbtn_fan_changed_f fan_changed_cb = NULL;
static uint8_t current_state = 0;

// Switch all relays off, then switch on the relay that matches val.
// val is rounded to the nearest level: 0, 33, 66 or 100.
static void apply_state(uint8_t val)
{
    uint8_t level;
    if (val == 0)
        level = 0;
    else if (val <= 33)
        level = 33;
    else if (val <= 66)
        level = 66;
    else
        level = 100;

    for (int i = 1; i < BTN_COUNT; i++)
        gpio_set_level(btn_map[i].relay_pin, RELAY_OFF);

    for (int i = 1; i < BTN_COUNT; i++)
    {
        if (btn_map[i].val == level)
            gpio_set_level(btn_map[i].relay_pin, RELAY_ON);
    }

    current_state = level;
    ESP_LOGI(TAG, "fan state = %d", level);
}

// Task: poll the buttons, detect a new press, update relays, notify the cloud
static void button_task(void *param)
{
    int last[BTN_COUNT];
    for (int i = 0; i < BTN_COUNT; i++)
        last[i] = !BTN_PRESSED;

    while (1)
    {
        for (int i = 0; i < BTN_COUNT; i++)
        {
            int level = gpio_get_level(btn_map[i].btn_pin);
            if (level == BTN_PRESSED && last[i] != BTN_PRESSED)
            {
                ESP_LOGI(TAG, "button %d pressed", i);
                apply_state(btn_map[i].val);
                if (fan_changed_cb)
                    fan_changed_cb(current_state);   // -> appaws_publish()
            }
            last[i] = level;
        }
        vTaskDelay(POLL_MS / portTICK_PERIOD_MS);
    }
}

void apphw_init(appbtn_fan_changed_f cb)
{
    fan_changed_cb = cb;

    // Buttons: inputs with internal pull-up
    for (int i = 0; i < BTN_COUNT; i++)
    {
        gpio_reset_pin(btn_map[i].btn_pin);
        gpio_set_direction(btn_map[i].btn_pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(btn_map[i].btn_pin, GPIO_PULLUP_ONLY);
    }

    // Relays: outputs, start OFF
    for (int i = 1; i < BTN_COUNT; i++)
    {
        gpio_reset_pin(btn_map[i].relay_pin);
        gpio_set_direction(btn_map[i].relay_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(btn_map[i].relay_pin, RELAY_OFF);
    }

    // Enable the logic converter output
    gpio_reset_pin(APP_OE);
    gpio_set_direction(APP_OE, GPIO_MODE_OUTPUT);
    gpio_set_level(APP_OE, 1);

    current_state = 0;
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
}

uint8_t apphw_get_state(void)
{
    return current_state;
}

// Called from the cloud side (voice/app request). Does NOT call the callback,
// otherwise the cloud would be notified about its own change.
void apphw_set_state(uint8_t val)
{
    apply_state(val);
}