#ifndef NAREDEF_GLOBALS
#define NAREDEF_GLOBALS
#include <cstdint>
#include "driver/gpio.h"
#include "esp_log.h"

/**
 * Current LED status. Use bitwise OR to combine states.
 * Button_handling | Streaming | Provisioning | Connected
 */
#define GLOBALSTAT_BUTTON_HANDLING 8
#define GLOBALSTAT_STREAMING 4
#define GLOBALSTAT_PROVISIONING 2
#define GLOBALSTAT_CONNECTED 1

uint8_t globalStatus = 0;

static uint16_t idle_counter = 30;

// GPIO definitions
#define GPIO_MOSI 10
#define GPIO_MISO 11
#define GPIO_SCLK 8
#define GPIO_CS1 13
#define GPIO_CS2 12
#define GPIO_CS3 9
#define PIN_BTN GPIO_NUM_0
#define PIN_LED GPIO_NUM_15
#define PIN_POW GPIO_NUM_14
#define PIN_BAT GPIO_NUM_18

#endif // NAREDEF_GLOBALS