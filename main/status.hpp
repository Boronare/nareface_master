#ifndef STATUS_HPP
#define STATUS_HPP

#include <cstdint>

enum ledStatusType:uint8_t{
    LEDS_OFF = 0,
    LEDS_WIFI_PROVISIONING = 1,
    LEDS_WIFI_CONNECTED = 2,
    LEDS_BUTTON_READING = 3,
};
uint8_t ledStatus = 0;
#endif