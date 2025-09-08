#ifndef NAREDEF_STATUS
#define NAREDEF_STATUS

#include "globals.h"


struct ledPattern{
    uint8_t turn_on100mss;
    uint8_t turn_off100mss;
    uint8_t repeat;
    uint8_t rest100mss;
};
ledPattern ledPatterns[] = {
    {0, 1, 1, 1},  // LEDS_OFF
    {5, 5, 3, 30},  // LEDS_WIFI_PROVISIONING
    {1, 1, 1, 30},  // LEDS_WIFI_CONNECTED
    {0, 0, 1, 0},  // should not occur
    {10, 0, 1, 0},  // LEDS_BOOTING
};
static void ledStatusTask(void* arg) {
    while (true) {
        if(button_handling) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ledStatusType status = ledStatus;
        ledPattern pattern = ledPatterns[status];
        for (uint8_t i = 0; i < pattern.repeat; i++) {
            // turn on LED
            gpio_set_level(PIN_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(100*pattern.turn_on100mss));
            // Turn off LEDs
            gpio_set_level(PIN_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(100*pattern.turn_off100mss));
            if (status != ledStatus) {
                break;
            }
        }
        for (uint8_t i = 0; i < pattern.rest100mss; i++) {
            if (status != ledStatus) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
#endif // NAREDEF_STATUS