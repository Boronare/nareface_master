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
    {1, 0, 1, 0},  // BOOTING
    {1, 1, 1, 10},  // WIFI_CONNECTED
    {5, 5, 3, 30},  // WIFI_PROVISIONING
    {1, 1, 1, 50},  // STREAMING
    {0, 0, 0, 1},  // BUTTON_HANDLING
};
static void ledStatusTask(void* arg) {
    while (true) {
        uint8_t status = 0;
        uint8_t oldStatus = globalStatus;
        if(globalStatus & GLOBALSTAT_BUTTON_HANDLING) status = 4;
        else if(globalStatus & GLOBALSTAT_STREAMING) status = 3;
        else if(globalStatus & GLOBALSTAT_PROVISIONING) status = 2;
        else if(globalStatus & GLOBALSTAT_CONNECTED) status = 1;
        ledPattern pattern = ledPatterns[status];
        for (uint8_t i = 0; i < pattern.repeat; i++) {
            // turn on LED
            gpio_set_level(PIN_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(100*pattern.turn_on100mss));
            // Turn off LEDs
            gpio_set_level(PIN_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(100*pattern.turn_off100mss));
            if (oldStatus != globalStatus) {
                break;
            }
        }
        for (uint8_t i = 0; i < pattern.rest100mss; i++) {
            if (oldStatus != globalStatus) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
#endif // NAREDEF_STATUS