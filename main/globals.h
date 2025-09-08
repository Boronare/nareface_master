#ifndef NAREDEF_GLOBALS
#define NAREDEF_GLOBALS
#include <cstdint>
#include "driver/gpio.h"
#include "esp_log.h"

enum ledStatusType:uint8_t{
    LEDS_OFF = 0,
    LEDS_WIFI_PROVISIONING = 1,
    LEDS_WIFI_CONNECTED = 2,
    LEDS_BOOTING = 4,
};
ledStatusType ledStatus = ledStatusType::LEDS_BOOTING;
uint8_t button_handling = 0;

// GPIO definitions
#define GPIO_MOSI 10
#define GPIO_MISO 11
#define GPIO_SCLK 8
#define GPIO_CS1 9
#define GPIO_CS3 13
#define GPIO_CS2 12
#define PIN_BTN GPIO_NUM_0
#define PIN_LED GPIO_NUM_15
#define PIN_POW GPIO_NUM_14
#define PIN_BAT GPIO_NUM_18

#endif // NAREDEF_GLOBALS