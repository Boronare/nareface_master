#include "esp_http_server.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "mdns.h"

#include "camera.hpp"
#include "wifi.hpp"
#include "button.hpp"
#include "status.hpp"


#define GPIO_MOSI 34
#define GPIO_MISO 35
#define GPIO_SCLK 18
#define GPIO_CS1 26
#define GPIO_CS2 33

// struct spijpeg_pack{
//     int32_t size;
//     uint8_t bytes[4092];
//   };
// constexpr int jpegbuf_size = sizeof(spijpeg_pack);

static const char* TAG      = "SPIMASTER";

SemaphoreHandle_t jpeg_mutex;
uint8_t flags;

// SPI handle
spi_device_handle_t spi_handle[2];

constexpr int target_fps = 24;
constexpr int target_frame_time = 1000 / target_fps;

spijpeg_pack* spi_buffer;
spijpeg_pack* jpeg_buffer;

// FPS calculation
uint8_t frame_count    = 0;
uint32_t last_fps_check = 0;
float    current_fps    = 0.0;


void update_fps() {
    frame_count++;
    uint32_t current_time = esp_timer_get_time() / 1000; // Convert microseconds to milliseconds
    uint32_t elapsed_time = current_time - last_fps_check;

    if (elapsed_time >= 1000) { // Calculate FPS every second
        current_fps = frame_count * 1000.0f / elapsed_time;
        frame_count = 0;
        last_fps_check = current_time;
        ESP_LOGI(TAG, "Current FPS: %.1f", current_fps);
    }
}
uint16_t lasttime[3] = {0};
void waitFrameTime(uint8_t devnum){
    uint16_t current_time = (esp_timer_get_time()/1000)%65536;
    uint16_t elapsed_time = current_time - lasttime[devnum];
    lasttime[devnum] = current_time;
    if(elapsed_time < target_frame_time){
        vTaskDelay(pdMS_TO_TICKS(target_frame_time - elapsed_time));
    }
}

// HTTP streaming
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=STREAM_BOUNDARY";
static const char* _STREAM_BOUNDARY     = "\r\n--STREAM_BOUNDARY\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t spistream_handler(httpd_req_t* req, uint8_t spi_num) {
    ESP_LOGI(TAG, "Stream handler called");
    esp_err_t res = ESP_OK;
    while (1) {
        waitFrameTime(spi_num);
        //check if httpd connection alive
        res = httpd_resp_sendstr_chunk(req, " ");
        if (res != ESP_OK) {
            ESP_LOGI(TAG, "Client disconnected, stopping stream CODE : %04x",res);
            break;
        }
        spi_transaction_t t;
        //get semaphore
        xSemaphoreTake(jpeg_mutex, portMAX_DELAY);

        memset(&t, 0, sizeof(t));
        t.length    = jpegbuf_size * 8; // 4 bytes * 8 bits
        t.rx_buffer = spi_buffer;

        esp_err_t ret = spi_device_transmit(spi_handle[spi_num], &t);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive JPEG size");
            xSemaphoreGive(jpeg_mutex);
            continue;
        }
        if(spi_buffer->size == 0){
            xSemaphoreGive(jpeg_mutex);
            continue;
        }
        if(spi_buffer->size > jpegbuf_size) {
            ESP_LOGE(TAG, "JPEG size too large: %d", (int)spi_buffer->size);
            xSemaphoreGive(jpeg_mutex);
            continue;
        }
        //validate if data is correct jpeg format
        if(spi_buffer->bytes[0] != 0xFF || spi_buffer->bytes[1] != 0xD8){
            ESP_LOGE(TAG, "JPEG data is not valid");
            xSemaphoreGive(jpeg_mutex);
            continue;
        }


        char   part_buf[64];

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, spi_buffer->size);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (char *)spi_buffer->bytes, spi_buffer->size);
        }
        xSemaphoreGive(jpeg_mutex);
        if (res != ESP_OK) {
            break;
        }
        update_fps();
    }

    return res;
}
static void leftstream_task(void* arg) {
    httpd_req_t* req = (httpd_req_t*)arg;
    spistream_handler(req, 0);
    httpd_resp_send_chunk(req, NULL, 0); // End the response
    httpd_req_async_handler_complete(req); // Complete the async handler
    flags &= ~(1<<0);
    vTaskDelete(NULL);
}
static void rightstream_task(void* arg) {
    httpd_req_t* req = (httpd_req_t*)arg;
    spistream_handler(req, 1);
    httpd_resp_send_chunk(req, NULL, 0); // End the response
    httpd_req_async_handler_complete(req); // Complete the async handler
    flags &= ~(1<<1);
    vTaskDelete(NULL);
}

static esp_err_t leftstream_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "Left stream handler called");
    if(flags&(1<<0)){
        ESP_LOGI(TAG, "Left stream already started");
        return ESP_OK;
    }
    if (httpd_resp_set_type(req, _STREAM_CONTENT_TYPE) != ESP_OK)
        return ESP_FAIL;
    httpd_req_t* copy = NULL;
    httpd_req_async_handler_begin(req, &copy);
    if (copy == NULL) {
        ESP_LOGE(TAG, "Failed to create copy of request");
        return ESP_FAIL;
    }
    if(xTaskCreate(leftstream_task, "left_task", 3*1024, copy, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create left stream task");
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    flags |= 1<<0;
    return ESP_OK;
}

static esp_err_t rightstream_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "Right stream handler called");
    if(flags&(1<<1)){
        ESP_LOGI(TAG, "Right stream already started");
        return ESP_OK;
    }
    if (httpd_resp_set_type(req, _STREAM_CONTENT_TYPE) != ESP_OK)
        return ESP_FAIL;
    httpd_req_t* copy = NULL;
    httpd_req_async_handler_begin(req, &copy);
    if (copy == NULL) {
        ESP_LOGE(TAG, "Failed to create copy of request");
        return ESP_FAIL;
    }
    if(xTaskCreate(rightstream_task, "right_task", 3*1024, copy, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create right stream task");
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    flags |= 1<<1;
    return ESP_OK;
}

static void facestream_task(void* arg){
    httpd_req_t* req = (httpd_req_t*)arg;
    ESP_LOGI(TAG, "Stream handler called");
    esp_err_t res = ESP_OK;

    while (1) {
        //check if httpd connection alive
        res = httpd_resp_sendstr_chunk(req, " ");
        if (res != ESP_OK) {
            ESP_LOGI(TAG, "Client disconnected, stopping stream CODE : %04x",res);
            break;
        }
        char   part_buf[64];
        // camera_fb_t * fb = NULL;
        // fb = esp_camera_fb_get();
        // if (!fb) {
        //     ESP_LOGE(TAG, "Camera capture failed");
        //     res = ESP_FAIL;
        //     continue;
        // }
        // if(fb->format != PIXFORMAT_JPEG){
        //     xSemaphoreTake(jpeg_mutex, portMAX_DELAY);
        //     bool jpeg_converted = newFrame2jpg(fb, jpeg_buffer->bytes, (int*)&jpeg_buffer->size);
        //     if(!jpeg_converted){
        //         ESP_LOGE(TAG, "JPEG compression failed");
        //         xSemaphoreGive(jpeg_mutex);
        //         esp_camera_fb_return(fb);
        //         continue;
        //     }
        // }
        // esp_camera_fb_return(fb);
        // xSemaphoreGive(jpeg_mutex);
        res = getCameraJpeg(jpeg_buffer->bytes, (int*)&jpeg_buffer->size);
        if(jpeg_buffer->size == 0){
            ESP_LOGE(TAG, "JPEG size is 0");
            continue;
        }
        if(jpeg_buffer->size > jpegbuf_size) {
            ESP_LOGE(TAG, "JPEG size too large: %d", (int)jpeg_buffer->size);
            continue;
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, jpeg_buffer->size);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (char *)jpeg_buffer->bytes, jpeg_buffer->size);
        }
        if (res != ESP_OK) {
            break;
        }
        update_fps();
    }
    httpd_resp_send_chunk(req, NULL, 0); // End the response
    httpd_req_async_handler_complete(req); // Complete the async handler
    flags &= ~(1<<2);
    vTaskDelete(NULL);
}
static esp_err_t face_stream_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "Face stream handler called");
    if(flags&(1<<2)){
        ESP_LOGI(TAG, "Face stream already started");
        return ESP_OK;
    }
    if (httpd_resp_set_type(req, _STREAM_CONTENT_TYPE) != ESP_OK)
        return ESP_FAIL;
    httpd_req_t* copy = NULL;
    httpd_req_async_handler_begin(req, &copy);
    if (copy == NULL) {
        ESP_LOGE(TAG, "Failed to create copy of request");
        return ESP_FAIL;
    }
    if(xTaskCreate(facestream_task, "face_task", 3*1024, copy, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create face stream task");
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    flags |= 1<<2;
    return ESP_OK;
}

#define PIN_POW GPIO_NUM_1
extern "C" void app_main(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_POW | (1ULL << PIN_LED));
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(PIN_POW, 1);
    gpio_set_level(PIN_LED, 1);

    io_conf.pin_bit_mask = (1ULL << PIN_BTN),
    io_conf.mode = GPIO_MODE_INPUT,
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE,
    io_conf.intr_type = GPIO_INTR_ANYEDGE,
    gpio_config(&io_conf);

    // GPIO 인터럽트 핸들러 설정
    gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    gpio_isr_handler_add((gpio_num_t)PIN_BTN, gpio_isr_handler, (void*)NULL);
    //make button_task
    if(xTaskCreatePinnedToCore(button_task, "button_task", 1024, NULL, 5, NULL, 1) != pdPASS) {
    }


    // configuration for the SPI bus
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num      = GPIO_MOSI;
    buscfg.miso_io_num      = GPIO_MISO;
    buscfg.sclk_io_num      = GPIO_SCLK;
    buscfg.quadwp_io_num    = -1;
    buscfg.quadhd_io_num    = -1;
    buscfg.max_transfer_sz  = jpegbuf_size;

    // configuration for the SPI device
    spi_device_interface_config_t devcfg[2] = {};
    devcfg[0].command_bits                  = 0;
    devcfg[0].address_bits                  = 0;
    devcfg[0].dummy_bits                    = 0;
    devcfg[0].clock_speed_hz                = 20000000;
    devcfg[0].duty_cycle_pos                = 32; // 128 = 50% duty cycle
    devcfg[0].mode                          = 0;
    devcfg[0].spics_io_num                  = GPIO_CS1;
    devcfg[0].cs_ena_posttrans              = 3; // keep the CS low 3 cycles after transaction
    devcfg[0].queue_size                    = 2;

    devcfg[1] = devcfg[0];
    devcfg[1].spics_io_num                  = GPIO_CS2;

    // initialize SPI bus and add device
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus");
        return;
    }

    ret = spi_bus_add_device(SPI2_HOST, &devcfg[0], &spi_handle[0]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device1");
        return;
    }
    ret = spi_bus_add_device(SPI2_HOST, &devcfg[1], &spi_handle[1]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device2");
        return;
    }

    ESP_LOGI(TAG, "SPI Master initialized successfully");


    // connect to WiFi
    connect_wifi();
    //Wait for wifi connection

    //initialize camera
    ret = initCam();
    if (ret != ESP_OK){
        ESP_LOGE(TAG, "Failed to initialize Camera:%04x",ret);
        // return;
    }

    // IMPORTANT!!!!!!: allocate DMA-capable memory for JPEG buffer
    jpeg_buffer = (spijpeg_pack*)heap_caps_malloc(jpegbuf_size,MALLOC_CAP_DMA);
    spi_buffer = (spijpeg_pack*)heap_caps_malloc(jpegbuf_size,MALLOC_CAP_DMA);
    if (!jpeg_buffer || !spi_buffer) {
        ESP_LOGE(TAG, "Failed to allocate JPEG buffer");
        return;
    }
    jpeg_mutex = xSemaphoreCreateMutex();
    if (!jpeg_mutex) {
        ESP_LOGE(TAG, "Failed to create JPEG mutex");
        return;
    }

    ESP_LOGI(TAG,"\nWiFi connected");

    // Initialize mDNS as nareface.local
    esp_err_t err = mdns_init();
    if (err) {
        ESP_LOGE(TAG, "mDNS Init failed: %s", esp_err_to_name(err));
        return;
    }
    err = mdns_hostname_set("nareface");
    if (err) {
        ESP_LOGE(TAG, "mDNS Hostname failed: %s", esp_err_to_name(err));
        return;
    }
    printf("test");

    // start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size     = 3*1024;

    httpd_handle_t stream_httpd = NULL;
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_uri_t stream_uri[3] = {
            {.uri = "/left", .method = HTTP_GET, .handler = leftstream_handler, .user_ctx = NULL},
            {.uri = "/right", .method = HTTP_GET, .handler = rightstream_handler, .user_ctx = NULL},
            {.uri = "/face", .method = HTTP_GET, .handler = face_stream_handler, .user_ctx = NULL}
        };
        httpd_register_uri_handler(stream_httpd, &stream_uri[0]);
        httpd_register_uri_handler(stream_httpd, &stream_uri[1]);
        httpd_register_uri_handler(stream_httpd, &stream_uri[2]);
    }
}