#ifndef BUTTON_HPP
#define BUTTON_HPP

#include <driver/gpio.h>

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"

#define BUTTON_GPIO GPIO_NUM_0  // 버튼이 연결된 GPIO 핀 번호

#define CLICK_TIMEOUT 500 // 클릭 감지 시간 (밀리초 단위)
#define LONG_PRESS_THRESHOLD 2000 // 2초 이상 눌렀을 때 (밀리초)
#define LONG_PRESS_4S_THRESHOLD 4000 // 4초 이상 눌렀을 때 (밀리초)

static uint8_t btn_status = 0; // 버튼 상태를 저장하는 변수

void IRAM_ATTR gpio_isr_handler(void* arg) {
    if (gpio_get_level(GPIO_NUM_0) == 0) {
        // set gpio1,2 level to 0
        btn_status = 1;
    }
}

void button_task(void* arg) {
    while(1){
        if (btn_status) {
            gpio_set_level(GPIO_NUM_1, 0);
            gpio_set_level(GPIO_NUM_2, 0);
            esp_deep_sleep_start(); // 깊은 슬립 모드로 전환
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // 10ms 대기
    }
}
#endif