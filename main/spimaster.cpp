#include "esp_http_server.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"

#include "wifi.hpp"
#include "status.hpp"

static const char* TAG      = "SPIMASTER";


#define GPIO_MOSI 10
#define GPIO_MISO 11
#define GPIO_SCLK 8
#define GPIO_CS1 9
#define GPIO_CS3 13
#define GPIO_CS2 12
#define PIN_BTN GPIO_NUM_0
#define PIN_LED GPIO_NUM_15
#define PIN_POW GPIO_NUM_14

uint8_t flags;

// SPI handle
spi_device_handle_t spi_handle[3];

struct spijpeg_pack{
  int32_t size;
  uint8_t bytes[4000];
};

constexpr int jpegbuf_size = sizeof(spijpeg_pack);

constexpr int target_fps = 60;
constexpr int target_frame_time = 1000 / target_fps;

spijpeg_pack* spi_buffer[3];
// spijpeg_pack* jpeg_buffer;

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

        memset(&t, 0, sizeof(t));
        t.length    = jpegbuf_size * 8; // 4 bytes * 8 bits
        t.rx_buffer = spi_buffer[spi_num];

        esp_err_t ret = spi_device_transmit(spi_handle[spi_num], &t);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive JPEG size");
            continue;
        }
        if(spi_buffer[spi_num]->size == 0){
            continue;
        }
        if(spi_buffer[spi_num]->size > jpegbuf_size) {
            ESP_LOGE(TAG, "JPEG size too large: %d", (int)spi_buffer[spi_num]->size);
            continue;
        }
        //validate if data is correct jpeg format
        if(spi_buffer[spi_num]->bytes[0] != 0xFF || spi_buffer[spi_num]->bytes[1] != 0xD8){
            ESP_LOGE(TAG, "JPEG data is not valid");
            continue;
        }


        char   part_buf[64];

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, spi_buffer[spi_num]->size);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (char *)spi_buffer[spi_num]->bytes, spi_buffer[spi_num]->size);
        }
        // xSemaphoreGive(jpeg_mutex);
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
    spistream_handler(req, 2);
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

extern "C" void app_main(void) {
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_POW | (1ULL << PIN_LED));
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << PIN_BTN);
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);

    gpio_set_level(PIN_LED, 1);
    gpio_set_level(PIN_POW, 0); // Power off the camera
    
    //wait until button(gpio 0) is pressed
    while(gpio_get_level(PIN_BTN) == 1){
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    gpio_set_level(PIN_LED, 0);


    // configuration for the SPI bus
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num      = GPIO_MOSI;
    buscfg.miso_io_num      = GPIO_MISO;
    buscfg.sclk_io_num      = GPIO_SCLK;
    buscfg.quadwp_io_num    = -1;
    buscfg.quadhd_io_num    = -1;
    buscfg.max_transfer_sz  = jpegbuf_size;

    // configuration for the SPI device
    spi_device_interface_config_t devcfg[3] = {};
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

    devcfg[2] = devcfg[0];
    devcfg[2].spics_io_num                  = GPIO_CS3;

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
    ret = spi_bus_add_device(SPI2_HOST, &devcfg[2], &spi_handle[2]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device3");
        return;
    }

    ESP_LOGI(TAG, "SPI Master initialized successfully");


    // connect to WiFi
    connect_wifi();
    for(int i = 0; i < 3; i++) {
        spi_buffer[i] = (spijpeg_pack*)heap_caps_malloc(jpegbuf_size, MALLOC_CAP_DMA);
        if (!spi_buffer[i]) {
            ESP_LOGE(TAG, "Failed to allocate SPI buffer");
            return;
        }
    }

    ESP_LOGI(TAG,"WiFi connected");

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
    gpio_set_level(PIN_POW, 1);
}