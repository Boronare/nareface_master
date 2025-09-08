#include "globals.h"

#ifndef NAREDEF_BUTTON
#define NAREDEF_BUTTON

//function pointers for button actions
void (*single_click_action)();
void (*double_click_action)();
void (*triple_click_action)();
void (*long_press_action)();

void button_task(void* arg) {
    constexpr int16_t LONG_MS = 3000;       // 3 seconds
    constexpr int16_t CLICK_WIN_MS = 700;   // time window for multi-click
    constexpr int16_t DEBOUNCE_MS = 40;     // debounce window

    // Debounce state
    bool raw = (gpio_get_level(PIN_BTN) == 0);
    bool debounced = raw;
    int64_t last_change = esp_timer_get_time() / 1000; // ms

    int64_t press_start = debounced ? last_change : 0;
    int clicks = 0;
    int64_t click_deadline = 0;
    for(;;){
        bool new_raw = (gpio_get_level(PIN_BTN) == 0);
        int64_t now = esp_timer_get_time()/1000;
        if (new_raw != raw) {
            raw = new_raw;
            last_change = now; // restart debounce window
        }
        // Update debounced state if stable
        if (debounced != raw && (now - last_change) >= DEBOUNCE_MS) {
            bool old = debounced;
            debounced = raw;
            // Edge processing based on debounced transitions
            if (debounced && !old) { // down edge
                press_start = now;
                gpio_set_level(PIN_LED, 1);
            }
            if (!debounced && old) { // up edge
                gpio_set_level(PIN_LED, 0);
                int64_t dur = now - press_start;
                if (dur < CLICK_WIN_MS && (clicks == 0 || now < click_deadline)) {
                    // short click
                    clicks++;
                    if (clicks == 1) {
                        click_deadline = now + CLICK_WIN_MS;
                    }
                    if (clicks >= 3) {
                        if(triple_click_action != nullptr)
                            triple_click_action();
                        clicks = 0;
                    }
                }
                press_start = 0;
            }
        }

        // Long-press while held (debounced)
        if (clicks == 0 && debounced && press_start && (now - press_start >= LONG_MS)) {
            if(long_press_action != nullptr)
                long_press_action();
            press_start = 0; // prevent repeat            
        }

        if (clicks > 0 && now > click_deadline) {
            if(clicks == 1){
                if(single_click_action != nullptr)
                    single_click_action();
            }if(clicks == 2){
                ESP_LOGI("BUTTON", "Double click");
                if(double_click_action != nullptr)
                    double_click_action();
            }
            clicks = 0;
        }
        // If nothing happened for a while, break
        if (clicks == 0 && !debounced) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI("BUTTON", "Button handling done");
    button_handling = 0;
    vTaskDelete(NULL);
}

//button ISR handler
void IRAM_ATTR button_isr_handler(void* arg) {
    if(button_handling) return; // already handling button
    button_handling = 1;
    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, NULL);
}

#endif // NAREDEF_BUTTON