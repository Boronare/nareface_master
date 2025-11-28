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

#define HIDDENSTAT_MDNS_INITIALIZED 1

uint8_t globalStatus = 0;
uint8_t hiddenStatus = 0;
uint16_t curBat = 0; // filtered battery voltage in mV

static uint16_t idle_counter = 30;

// // GPIO definitions for ESP32-S2
// #define GPIO_MOSI 10
// #define GPIO_MISO 11
// #define GPIO_SCLK 8
// #define GPIO_CS1 13
// #define GPIO_CS2 12
// #define GPIO_CS3 9
// #define PIN_BTN GPIO_NUM_0
// #define PIN_LED GPIO_NUM_15
// #define PIN_POW GPIO_NUM_14
// #define PIN_BAT GPIO_NUM_18

// GPIO definitions for ESP32-C3
#define GPIO_MOSI 7
// #define GPIO_MISO 2
// #define GPIO_SCLK 6
#define GPIO_MISO 6
#define GPIO_SCLK 2
#define GPIO_CS1 4
#define GPIO_CS2 10
#define GPIO_CS3 9
#define PIN_BTN GPIO_NUM_5
#define PIN_LEDW GPIO_NUM_8
#define PIN_LEDP GPIO_NUM_0
#define PIN_POW GPIO_NUM_1
#define PIN_BAT GPIO_NUM_3

//global function definitions
static void enter_deep_sleep_wait_button();

#endif // NAREDEF_GLOBALS