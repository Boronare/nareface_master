#ifndef NAREDEF_STATUS
#define NAREDEF_STATUS

#include "globals.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"


struct ledPattern{
    uint8_t turn_on100mss;
    uint8_t turn_off100mss;
    uint8_t repeat;
    uint8_t rest100mss;
};
ledPattern ledPatterns[] = {
    {1, 0, 1, 0},  // BOOTING
    {1, 1, 1, 10},  // WIFI_CONNECTED
    {1, 1, 10, 1},  // WIFI_PROVISIONING
    {1, 1, 1, 50},  // STREAMING
    {0, 0, 0, 1},  // BUTTON_HANDLING
};

// /status endpoint (battery, etc.)
static adc_oneshot_unit_handle_t s_adc_unit = nullptr;
static adc_cali_handle_t s_adc_cali = nullptr;
static bool s_adc_inited = false;
static bool s_adc_cali_inited = false;
static adc_unit_t   s_adc_unit_id;
static adc_channel_t s_adc_channel;

static bool adc_cali_init_once(adc_unit_t unit, adc_atten_t atten) {
    if (s_adc_cali_inited) return true;
    bool calibrated = false;

    // Try line fitting first (supported on ESP32-S2)
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal_cfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cal_cfg, &s_adc_cali) == ESP_OK) {
        calibrated = true;
    }
#endif

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cal_cfg_cf = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cal_cfg_cf, &s_adc_cali) == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    s_adc_cali_inited = calibrated;
    return calibrated;
}

static void ensure_adc_init() {
    if (s_adc_inited) return;
    // Map GPIO to ADC unit/channel
    if (adc_oneshot_io_to_channel((int)PIN_BAT, &s_adc_unit_id, &s_adc_channel) != ESP_OK) {
        ESP_LOGE("STAT", "PIN_BAT GPIO %d is not ADC-capable on this chip", (int)PIN_BAT);
        // Fallback to ADC_UNIT_1 CH0 to avoid crash, but will read 0
        s_adc_unit_id = ADC_UNIT_1;
        s_adc_channel = (adc_channel_t)0;
    }
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = s_adc_unit_id,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_unit));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_2_5, // up to ~3.6V FS on pin (we're <1V due to divider)
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_unit, s_adc_channel, &chan_cfg));

    // Try to init calibration (optional)
    adc_cali_init_once(s_adc_unit_id, chan_cfg.atten);
    s_adc_inited = true;
}

static uint16_t read_battery_voltage_mv() {
    ensure_adc_init();
    int raw = 0;
    int mv = 0;
    if (adc_oneshot_read(s_adc_unit, s_adc_channel, &raw) != ESP_OK) {
        return 0;
    }
    if (s_adc_cali_inited && s_adc_cali) {
        if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
        }
    }
    if (mv <= 0) {
        // Fallback approximation for 2.5 dB attenuation
        mv = raw * 1050 / 4095;
    }
    mv = mv * 37 / 36; // calibration adjustment
    // Divider: BAT -- 390k -- ADC -- 100k -- GND, Vbat = Vadc * (390+100)/100 = *4.9
    return mv * 49 / 10;
}

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
            // turn on LED (active-low)
            gpio_set_level(PIN_LEDW, 0);
            vTaskDelay(pdMS_TO_TICKS(100*pattern.turn_on100mss));
            // Turn off LEDs (inactive high)
            gpio_set_level(PIN_LEDW, 1);
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

static void powStatusTask(void* arg) {
    while (true) {
        //get voltage level from pin ADC
        int16_t vbat = read_battery_voltage_mv();
        // put filter then get result
        if(!curBat) curBat = vbat;
        curBat = (curBat*7 + vbat)/8;
        uint8_t i;
        // vbat level have 5 levels : >4.0V, 3.7~4.0V, 3.5~3.7V, 3.3~3.5V, <3.3V
        if (curBat > 3900) {
            i=3;
        } else if (curBat > 3700) {
            i=2;
        } else if (curBat > 3500) {
            i=1;
        } else if (curBat > 3300) {
            i=10;
        } else {
            //deep sleep
            enter_deep_sleep_wait_button();
            i=10;
        }
        // blink LED according to battery level
        vTaskDelay(pdMS_TO_TICKS(2000-200*i));
        for (; i > 0; i--) {
            gpio_set_level(PIN_LEDP, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(PIN_LEDP, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// --- Button handling ---
static void enter_deep_sleep_wait_button() {
    // ESP_LOGI(TAG, "Entering deep sleep. Hold BTN for 3s to wake.");
    // Configure wake on BTN low level (ESP32-C3 uses GPIO wake)
    for(int8_t i=0;i<100;i++){
        // flash LEDW briefly before sleeping (active-low)
        gpio_set_level(PIN_LEDW, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // ESP32-C3 doesn't support ext0, use GPIO wake instead
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(1ULL<<PIN_BTN, ESP_GPIO_WAKEUP_GPIO_LOW));
    
    // Ensure LEDW is off (inactive high) before sleeping
    gpio_set_level(PIN_LEDW, 1);
    gpio_set_level(PIN_LEDP, 1);
    gpio_set_level(PIN_POW, 0);
    // ESP_LOGI(TAG, "Entering light sleep. Hold BTN for 1s to reboot, release to sleep again.");
    esp_deep_sleep_start();
    esp_restart();
}
#endif // NAREDEF_STATUS