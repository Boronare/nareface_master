#include "esp_http_server.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "mdns.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "button.h"

#include "globals.h"
#include "wifi.hpp"
#include "status.hpp"

// Externally embedded binary files (linked via CMakeLists.txt)
extern const uint8_t jpeg_fallback_source_jpg_start[] asm("_binary_jpeg_fallback_source_jpg_start");
extern const uint8_t jpeg_fallback_source_jpg_end[]   asm("_binary_jpeg_fallback_source_jpg_end");

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Convenience pointers for compatibility
static const unsigned char* jpeg_sample = jpeg_fallback_source_jpg_start;
static const size_t jpeg_sample_size = jpeg_fallback_source_jpg_end - jpeg_fallback_source_jpg_start;

static const char* HTML_INDEX = (const char*)index_html_start;

static const char* TAG      = "SPIMASTER";

uint8_t flags;
// Forward declaration for deep sleep helper used by timer task

// Per-stream control to support replacing an active stream with a new one
static volatile bool       stream_stop[3] = { false, false, false };
static TaskHandle_t        stream_task_handle[3] = { nullptr, nullptr, nullptr };



void idle_timer_task(void* arg) {
    while(1){
        //if mdns initialized
        if(hiddenStatus & HIDDENSTAT_MDNS_INITIALIZED) {
            mdns_service_txt_item_set("_http", "_tcp", "status", "active");
        }
        if(idle_counter > 0) idle_counter--;
        else enter_deep_sleep_wait_button();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
static inline void idle_timer_cancel() { idle_counter = 65535; } //Sleep timer to 182hrs(Max)

static inline void idle_timer_arm_5min() { if(!flags) idle_counter = 30; }

// SPI handle
spi_device_handle_t spi_handle[3];

struct spijpeg_pack{
  uint16_t size;
  uint8_t bytes[5000];
};

constexpr int jpegbuf_size = sizeof(spijpeg_pack);

constexpr int target_frame_time = 45; // ms per frame (approx)

spijpeg_pack* spi_buffer[3];
// SemaphoreHandle_t jpeg_semaphore;

// FPS calculation
// uint8_t frame_count    = 0;
// uint32_t last_fps_check = 0;
// float    current_fps    = 0.0;


void update_fps() {
    // frame_count++;
    // uint32_t current_time = esp_timer_get_time() / 1000; // Convert microseconds to milliseconds
    // uint32_t elapsed_time = current_time - last_fps_check;

    // if (elapsed_time >= 1000) { // Calculate FPS every second
    //     current_fps = frame_count * 1000.0f / elapsed_time;
    //     ESP_LOGI(TAG, "Current FPS: %.1f / Memory Available : %d bytes", current_fps, heap_caps_get_free_size(MALLOC_CAP_8BIT));
    //     frame_count = 0;
    //     last_fps_check = current_time;
    // }
}
uint16_t lasttime[3] = {0};
void waitFrameTime(uint8_t devnum){
    uint16_t current_time = (esp_timer_get_time()/1000)%65536;
    uint16_t elapsed_time = current_time - lasttime[devnum];
    if(elapsed_time < target_frame_time){
        // ESP_LOGI(TAG, "Frame time short: %d ms, delaying %d ms", elapsed_time, target_frame_time - elapsed_time);
        vTaskDelay(pdMS_TO_TICKS(target_frame_time - elapsed_time));
    }
    lasttime[devnum] = (esp_timer_get_time()/1000)%65536;
}

// HTTP streaming
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
static const char* _STREAM_BOUNDARY     = "\r\n--123456789000000000000987654321\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

static esp_err_t ota_post_handler(httpd_req_t* req);
// --- Provisioning HTTP handlers ---
static esp_err_t index_get_handler(httpd_req_t* req){
    // Arm idle sleep timer on non-stream request
    idle_timer_arm_5min();
    httpd_resp_set_type(req, "text/html");
    // Stream in chunks to reduce memory pressure
    // Prepend embedded scan results as window.__APS
    httpd_resp_sendstr_chunk(req, "<script>");
    if(s_ap_running) {
        wifi_scan_now();
        const wifi_ap_record_t* recs = wifi_scan_records();
        uint16_t n = wifi_scan_count();
        httpd_resp_sendstr_chunk(req, "window.__APS=");
        httpd_resp_sendstr_chunk(req, "[");
        for (uint16_t i=0;i<n;i++){
            char ssid[33]={0}; memcpy(ssid, recs[i].ssid, 32);
            char item[96];
            uint8_t len = snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}", i?",":"", ssid, recs[i].rssi);
            httpd_resp_send_chunk(req, item, len);
        }
        httpd_resp_sendstr_chunk(req, "];");
    }
        uint8_t soc = 0;
    // if (vbat <= 3.3f) soc = 0;
    // else if (vbat >= 4.2f) soc = 100;
    // else soc = (vbat - 3.3f) / (4.2f - 3.3f) * 100.0f * vbat/4.2; // simple linear
    //1% step lookup table
    {
        static const uint16_t soc_table[100] = {
            // 1% ~ 10%
            3320, 3340, 3370, 3390, 3410, 3430, 3450, 3470, 3480, 3500,
            // 11% ~ 20%
            3510, 3520, 3540, 3550, 3560, 3570, 3580, 3590, 3600, 3610,
            // 21% ~ 30%
            3620, 3630, 3635, 3640, 3650, 3660, 3665, 3670, 3675, 3680,
            // 31% ~ 40%
            3690, 3695, 3700, 3705, 3710, 3720, 3725, 3730, 3735, 3740,
            // 41% ~ 50%
            3750, 3755, 3760, 3765, 3770, 3780, 3785, 3790, 3795, 3800,
            // 51% ~ 60%
            3810, 3815, 3820, 3825, 3830, 3840, 3845, 3850, 3855, 3860,
            // 61% ~ 70%
            3870, 3875, 3880, 3885, 3890, 3900, 3905, 3910, 3915, 3920,
            // 71% ~ 80%
            3930, 3940, 3945, 3950, 3960, 3970, 3975, 3980, 3990, 4000,
            // 81% ~ 90%
            4010, 4020, 4025, 4030, 4040, 4050, 4060, 4070, 4090, 4100,
            // 91% ~ 100%
            4110, 4120, 4130, 4140, 4150, 4160, 4170, 4180, 4190, 4200
        };
        for(int8_t i=9;i>=0;i--){
            if(curBat >= soc_table[i*10]){
                for(int8_t j=9;j>=0;j--){
                    int idx = i*10 + j;
                    if(curBat >= soc_table[idx]){
                        soc = idx + 1;
                        break;
                    }
                }
                break;
            }
        }
        char buf[64];
        uint8_t len = snprintf(buf, sizeof(buf), "window.__VBAT=%d;window.__SOC=%d;window.__MDNS=\"%s\";", curBat, soc, mdnsServiceName);
        httpd_resp_send_chunk(req, buf, len );
    }
    httpd_resp_sendstr_chunk(req, "</script>");
    
    const char* p = HTML_INDEX;
    while (*p) {
        size_t len = strlen(p);
        size_t chunk = len > 512 ? 512 : len;
        if (httpd_resp_send_chunk(req, p, chunk) != ESP_OK) {
            return ESP_FAIL;
        }
        p += chunk;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ...existing code...

// --- Provisioning HTTP handlers ---
// URL decode helper for application/x-www-form-urlencoded
static inline int _hex4(char c){
    if (c>='0' && c<='9') return c-'0';
    if (c>='A' && c<='F') return c-('A'-10);
    if (c>='a' && c<='f') return c-('a'-10);
    return -1;
}
static size_t url_decode_to(char* dst, size_t dst_cap, const char* src, size_t src_len){
    size_t di=0;
    for(size_t i=0;i<src_len && di+1<dst_cap;){
        char c=src[i];
        if (c=='+'){ dst[di++]=' '; i++; }
        else if (c=='%' && i+2<src_len){
            int hi=_hex4(src[i+1]), lo=_hex4(src[i+2]);
            if (hi>=0 && lo>=0){ dst[di++]=(char)((hi<<4)|lo); i+=3; }
            else { dst[di++]=c; i++; }
        } else { dst[di++]=c; i++; }
    }
    dst[di]='\0';
    return di;
}

static esp_err_t provision_post_handler(httpd_req_t* req){
    // Arm idle sleep timer on non-stream request
    idle_timer_arm_5min();
    // Read x-www-form-urlencoded body (read full content_len or up to buffer size)
    char content[256]={0};
    size_t total = 0, want = req->content_len;
    while (total < sizeof(content)-1 && total < want){
        int r = httpd_req_recv(req, content+total, (want-total) < (sizeof(content)-1-total) ? (want-total) : (sizeof(content)-1-total));
        if (r <= 0) break;
        total += r;
    }
    if (total == 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");

    // Parse ssid=...&pass=... and URL-decode
    char ssid[33]={0}; char pass[65]={0}; char mdns[33]={0};
    const char* p = strstr(content, "ssid=");
    if (p){
        p += 5; const char* e = p; while (*e && *e!='&') e++;
        url_decode_to(ssid, sizeof(ssid), p, (size_t)(e-p));
    }
    p = strstr(content, "pass=");
    if (p){
        p += 5; const char* e = p; while (*e && *e!='&') e++;
        url_decode_to(pass, sizeof(pass), p, (size_t)(e-p));
    }
    p = strstr(content, "mdns=");
    if (p){
        p += 5; const char* e = p; while (*e && *e!='&') e++;
        url_decode_to(mdns, sizeof(mdns), p, (size_t)(e-p));
    }
    softap_manual_start = 0;
    if(strlen(ssid) > 0){
        wifi_save_credentials_to_nvs(ssid, pass);
        wifi_set_credentials_and_connect(ssid, pass);
    }
    if(strlen(mdns) > 0){
        esp_err_t err = nvs_flash_init_partition("nvs");
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase_partition("nvs"));
            err = nvs_flash_init_partition("nvs");
        }
        if (err != ESP_OK) return err;
        nvs_handle_t my_handle = 0;
        err = nvs_open("storage", NVS_READWRITE, &my_handle);
        if (err != ESP_OK) return err;
        err = nvs_set_str(my_handle, "mdns_name", mdns);
        if (err == ESP_OK) {
            err = nvs_commit(my_handle);
        }
        nvs_close(my_handle);
        free(mdnsServiceName);
        mdnsServiceName = strdup(mdns);
        mdns_hostname_set(mdns);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t spistream_handler(httpd_req_t* req, uint8_t spi_num) {
    ESP_LOGI(TAG, "Stream handler called : %d", spi_num);
    uint8_t once_stream = 0; // check for replacement image stream
    esp_err_t res = ESP_OK;
    while (1) {
        // Allow graceful replacement: stop if requested
        if (stream_stop[spi_num]) {
            ESP_LOGI(TAG, "Stop requested for stream %u", spi_num);
            res = ESP_FAIL;
            break;
        }
        waitFrameTime(spi_num);
        //check if httpd connection alive
        res = httpd_resp_sendstr_chunk(req, " ");
        if (res != ESP_OK) {
            ESP_LOGI(TAG, "Client disconnected, stopping stream CODE : %04x",res);
            res = ESP_FAIL;
            break;
        }
        spi_transaction_t t;
        // xSemaphoreTake(jpeg_semaphore, portMAX_DELAY);
        memset(&t, 0, sizeof(t));
        t.length    = jpegbuf_size * 8; // 4 bytes * 8 bits
        t.rx_buffer = spi_buffer[spi_num];

        esp_err_t ret = spi_device_transmit(spi_handle[spi_num], &t);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive JPEG size");
            if(once_stream< 7 ){
                ESP_LOGW(TAG, "No SPI data, using sample JPEG");
                memcpy(spi_buffer[spi_num]->bytes, jpeg_sample, jpeg_sample_size);
                spi_buffer[spi_num]->size = jpeg_sample_size;
                once_stream &= ~1;
            }
            else{
                // xSemaphoreGive(jpeg_semaphore);
                continue;
            }
        }else{
            once_stream |= 1;
        }
        if(spi_buffer[spi_num]->size == 0){
            // No data from SPI, use sample JPEG for testing
            ESP_LOGW(TAG, "No SPI data, using sample JPEG");
            // memcpy(spi_buffer[spi_num]->bytes, jpeg_sample, jpeg_sample_size);
            // spi_buffer[spi_num]->size = jpeg_sample_size;
            //xSemaphoreGive(jpeg_semaphore);
            continue;
        }
        if(spi_buffer[spi_num]->size > jpegbuf_size) {
            
            if(spi_buffer[spi_num]->size == 65535 && once_stream < 7){
                once_stream &= ~2;
                ESP_LOGW(TAG, "No SPI data, using sample JPEG");
                memcpy(spi_buffer[spi_num]->bytes, jpeg_sample, jpeg_sample_size);
                spi_buffer[spi_num]->size = jpeg_sample_size;
            }
            else{
                ESP_LOGE(TAG, "JPEG size from SPI too large: %u", spi_buffer[spi_num]->size);
                // xSemaphoreGive(jpeg_semaphore);
                continue;
            }
        }else{
            once_stream |= 2;
        }
        //validate if data is correct jpeg format
        if(spi_buffer[spi_num]->bytes[0] != 0xFF || spi_buffer[spi_num]->bytes[1] != 0xD8){
            ESP_LOGE(TAG, "JPEG data is not valid, using sample JPEG");
            // memcpy(spi_buffer[spi_num]->bytes, jpeg_sample, jpeg_sample_size);
            // spi_buffer[spi_num]->size = jpeg_sample_size;
            // xSemaphoreGive(jpeg_semaphore);
            continue;
        }
        once_stream |= 4;


        char   part_buf[128];

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf(part_buf, 128, _STREAM_PART, spi_buffer[spi_num]->size, (int)(esp_timer_get_time()/1000000), (int)(esp_timer_get_time()%1000000));

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            //send jpeg data chunks by 64bytes separated
            size_t offset = 0;
            while (offset < spi_buffer[spi_num]->size) {
                size_t chunk_size = spi_buffer[spi_num]->size - offset;
                if (chunk_size > 512) {
                    chunk_size = 512;
                }
                res = httpd_resp_send_chunk(req, (char *)spi_buffer[spi_num]->bytes + offset, chunk_size);
                if (res != ESP_OK) {
                    continue;
                }
                offset += chunk_size;
            }
        }
        update_fps();
        // xSemaphoreGive(jpeg_semaphore);
    }

    return res;
}
// Unified stream task argument
struct StreamTaskArg { httpd_req_t* req; uint8_t index; };

static void stream_task(void* arg) {
    StreamTaskArg* a = (StreamTaskArg*)arg;
    httpd_req_t* req = a->req;
    uint8_t idx = a->index;

    // Prepare for fresh streaming session
    stream_stop[idx] = false;
    flags |= 1<<idx;
    gpio_set_level(PIN_POW, 1);
    globalStatus |= GLOBALSTAT_STREAMING; // streaming

    esp_err_t res = spistream_handler(req, idx);
    
    httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    
    
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow time for client to receive final data
    
    flags &= ~(1<<idx);
    if(!flags) {
        // gpio_set_level(PIN_POW, 0);
        // Stream ended and no other streams active: arm 5-minute idle sleep
        idle_timer_arm_5min();
        globalStatus &= ~GLOBALSTAT_STREAMING; // not streaming
    }
    // Mark task handle cleared
    stream_task_handle[idx] = nullptr;
    if(a) free(a);
    // ESP_LOGI(TAG, "Stream task %u ended", idx);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler_common(httpd_req_t* req) {
    uint8_t idx = (uint8_t)(uintptr_t)req->user_ctx; // 0:left,1:right,2:face
    if (flags & (1<<idx)) {
        ESP_LOGI(TAG, "Stream %u already started; requesting previous to stop", idx);
        stream_stop[idx] = true;
        TickType_t start = xTaskGetTickCount();
        // Wait up to ~1.5s for previous stream to exit
        while ((flags & (1<<idx)) && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(1500)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (flags & (1<<idx)) {
            ESP_LOGW(TAG, "Previous stream %u did not stop in time; proceeding to start new stream", idx);
        }
        // Clear stop flag for the new session
        stream_stop[idx] = false;
    }
    // Cancel idle sleep while streaming
    idle_timer_cancel();

    if (httpd_resp_set_type(req, _STREAM_CONTENT_TYPE) != ESP_OK) return ESP_FAIL;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "X-Framerate", "30");
    
    httpd_req_t* copy = NULL;
    httpd_req_async_handler_begin(req, &copy);
    if (!copy) {
        ESP_LOGE(TAG, "Failed to create copy of request");
        return ESP_FAIL;
    }
    
    StreamTaskArg* a = (StreamTaskArg*)malloc(sizeof(StreamTaskArg));
    if (!a) { httpd_req_async_handler_complete(copy); return ESP_FAIL; }
    a->req = copy; a->index = idx;
    if (xTaskCreate(stream_task, "stream_task", 3*1024, a, 5, &stream_task_handle[idx]) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stream task");
        httpd_req_async_handler_complete(copy);
        if(a) free(a);
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" void app_main(void) {
    // Mark current app as valid (for rollback support)
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA, marking app as valid");
            // After successful boot and basic checks, mark as valid
            // You might want to delay this until WiFi connects or other health checks pass
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    
    gpio_set_direction(PIN_POW, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_LEDW, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_LEDP, GPIO_MODE_OUTPUT);
    gpio_config_t gpioconfig = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&gpioconfig);

    gpio_set_level(PIN_POW, 0); // Power off the camera
    gpio_set_level(PIN_LEDP, 0); // Turn on power LED

    // Log reset reason and, if woke from deep sleep via button, require 3s hold to proceed
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", (int)rr);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "Wake from deep sleep: hold BTN for 3s to boot");
        int64_t start = esp_timer_get_time();
        while (gpio_get_level(PIN_BTN) == 0) {
            // show LEDW as lit when holding button (active-low)
            if ((esp_timer_get_time() - start) >= 1000000) gpio_set_level(PIN_LEDW, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if ((esp_timer_get_time() - start) < 1000000) {
            ESP_LOGW(TAG, "Hold not long enough, back to deep sleep");
            enter_deep_sleep_wait_button();
        }
        //reboot
        ESP_LOGI(TAG, "Booting up");
        esp_restart();
    }

    // Set default button actions
    {
        long_press_action = enter_deep_sleep_wait_button;
        triple_click_action = [](){
            ESP_LOGI(TAG, "Triple-click: toggle SoftAP");
            softap_manual_start = !softap_manual_start;
            if (softap_manual_start) start_softap();
            else stop_softap();
        };
    }
    ESP_LOGI(TAG, "Button configured: long-press to sleep, triple-click to SoftAP");

    // invoke interrupt when button is pressed
    gpio_set_intr_type(PIN_BTN, GPIO_INTR_NEGEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_BTN, button_isr_handler, (void*)PIN_BTN);

    ESP_LOGI(TAG, "Initializing SPI Master");
    

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
    devcfg[0].clock_speed_hz                = 5000000;
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

    // JPEG 버퍼 할당
    for (int i = 0; i < 3; i++) {
        spi_buffer[i] = (spijpeg_pack*)heap_caps_malloc(jpegbuf_size, MALLOC_CAP_DMA);
        if (!spi_buffer[i]) {
            ESP_LOGE(TAG, "Failed to allocate SPI buffer");
            return;
        }
    }
    // jpeg_semaphore = xSemaphoreCreateMutex();
    gpio_set_level(PIN_POW, 1); // Turn on power to camera modules
    vTaskDelay(pdMS_TO_TICKS(1000)); // wait for camera modules to power up
        //test jpeg spi read from spi devices
        spi_transaction_t trans;
        memset(&trans, 0, sizeof(trans));
        for(int i=2; i>=0; i--){
            trans.length = jpegbuf_size * 8; // 4 bytes * 8 bits
            trans.rx_buffer = spi_buffer[i];
            esp_err_t ret = spi_device_transmit(spi_handle[i], &trans);
            if (ret != ESP_OK || spi_buffer[i]->size == 0 || spi_buffer[i]->size > jpegbuf_size ||
                spi_buffer[i]->bytes[0] != 0xFF || spi_buffer[i]->bytes[1] != 0xD8) {
                ESP_LOGE(TAG, "SPI device %d test read failed", i);
                //blink both LED i+1 times to indicate error
                for(int j=0;j<i+2;j++){
                    gpio_set_level(PIN_LEDP, 0);
                    gpio_set_level(PIN_LEDW, 1);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    gpio_set_level(PIN_LEDP, 1);
                    gpio_set_level(PIN_LEDW, 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                gpio_set_level(PIN_LEDW, 1); //keep LEDW on to indicate error
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        gpio_set_level(PIN_LEDW, 0); //turn off LEDW if no error

    // Start button handler
    xTaskCreate(ledStatusTask, "led_status_task", 1024, NULL, 5, NULL);
    xTaskCreate(idle_timer_task, "idle_timer_task", 1024, NULL, 5, NULL);
    xTaskCreate(powStatusTask, "pow_status_task", 1024, NULL, 5, NULL);

    // connect to WiFi
    connect_wifi();

    ESP_LOGI(TAG,"WiFi connected");

    // start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = 3*1024; // avoid stack overflow in handlers
    config.lru_purge_enable = true;   // free idle sessions when sockets scarce
    config.send_wait_timeout = 20;    // wait for send buffer to reduce EAGAIN
    config.recv_wait_timeout = 10;

    httpd_handle_t stream_httpd = NULL;
    constexpr uint8_t uri_handlers = 6 ;
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_uri_t stream_uri[uri_handlers] = {
            {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL},
            {.uri = "/provision", .method = HTTP_POST, .handler = provision_post_handler, .user_ctx = NULL},
            {.uri = "/left", .method = HTTP_GET, .handler = stream_handler_common, .user_ctx = (void*)0},
            {.uri = "/right", .method = HTTP_GET, .handler = stream_handler_common, .user_ctx = (void*)1},
            {.uri = "/face", .method = HTTP_GET, .handler = stream_handler_common, .user_ctx = (void*)2},
            {.uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = NULL}
        };
        for(int i=0;i<uri_handlers;i++)
            httpd_register_uri_handler(stream_httpd, &stream_uri[i]);
    // Arm initial idle sleep timer once server is up
    idle_timer_arm_5min();
    }
}
static esp_err_t ota_post_handler(httpd_req_t* req) {
    idle_timer_cancel();

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No OTA partition available");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        idle_timer_arm_5min();
        return ESP_FAIL;
    }

    // Get currently running partition for rollback info
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            // Current image is still pending verification
            ESP_LOGW(TAG, "Current image pending verify, marking as valid first");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        idle_timer_arm_5min();
        return ESP_FAIL;
    }

    // Set response type early for chunk sending
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr_chunk(req, "OTA: start\n");

    size_t remaining = req->content_len;
    size_t written = 0;
    uint8_t* buf = (uint8_t*)malloc(4096);
    if (!buf) {
        esp_ota_end(handle);
        httpd_resp_sendstr_chunk(req, "ERR: malloc failed\n");
        httpd_resp_send_chunk(req, NULL, 0);
        idle_timer_arm_5min();
        return ESP_FAIL;
    }

    bool header_checked = false;
    const char* EXPECTED_PROJECT_NAME = "nareface";

    while (remaining > 0) {
        int to_read = remaining > 4096 ? 4096 : (int)remaining;
        int r = httpd_req_recv(req, (char*)buf, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            ESP_LOGE(TAG, "recv failed: %d", r);
            esp_ota_abort(handle);
            free(buf);
            httpd_resp_sendstr_chunk(req, "ERR: recv failed\n");
            httpd_resp_send_chunk(req, NULL, 0);
            idle_timer_arm_5min();
            return ESP_FAIL;
        }

        // Verify header on first chunk
        if (!header_checked && written == 0 && r >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
            esp_image_header_t img_hdr;
            memcpy(&img_hdr, buf, sizeof(esp_image_header_t));
            
            // Validate magic
            if (img_hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
                ESP_LOGE(TAG, "Invalid image magic: 0x%02X", img_hdr.magic);
                esp_ota_abort(handle);
                free(buf);
                httpd_resp_sendstr_chunk(req, "ERR: invalid image magic\n");
                httpd_resp_send_chunk(req, NULL, 0);
                idle_timer_arm_5min();
                return ESP_FAIL;
            }

            // Validate chip ID
            esp_chip_id_t expected_chip = ESP_CHIP_ID_ESP32C3;
            if (img_hdr.chip_id != expected_chip) {
                ESP_LOGE(TAG, "Wrong chip ID: %d (expected %d)", img_hdr.chip_id, expected_chip);
                esp_ota_abort(handle);
                free(buf);
                char msg[128];
                snprintf(msg, sizeof(msg), "ERR: wrong chip (got %d, need %d)\n", img_hdr.chip_id, expected_chip);
                httpd_resp_sendstr_chunk(req, msg);
                httpd_resp_send_chunk(req, NULL, 0);
                idle_timer_arm_5min();
                return ESP_FAIL;
            }

            // Parse app descriptor (contains project name)
            // Skip image header and first segment header to find app_desc
            size_t desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            if ((size_t)r >= desc_offset + sizeof(esp_app_desc_t)) {
                esp_app_desc_t app_desc;
                memcpy(&app_desc, buf + desc_offset, sizeof(esp_app_desc_t));
                
                // Verify project name
                if (strncmp(app_desc.project_name, EXPECTED_PROJECT_NAME, sizeof(app_desc.project_name)) != 0) {
                    ESP_LOGE(TAG, "Wrong project name: '%s' (expected '%s')", app_desc.project_name, EXPECTED_PROJECT_NAME);
                    esp_ota_abort(handle);
                    free(buf);
                    char msg[128];
                    snprintf(msg, sizeof(msg), "ERR: wrong project (got '%s', need '%s')\n", 
                             app_desc.project_name, EXPECTED_PROJECT_NAME);
                    httpd_resp_sendstr_chunk(req, msg);
                    httpd_resp_send_chunk(req, NULL, 0);
                    idle_timer_arm_5min();
                    return ESP_FAIL;
                }

                ESP_LOGI(TAG, "Image validated: project=%s, version=%s, chip=%d", 
                         app_desc.project_name, app_desc.version, img_hdr.chip_id);
                char info[128];
                snprintf(info, sizeof(info), "OK: validated (project=%s, ver=%s)\n", 
                         app_desc.project_name, app_desc.version);
                httpd_resp_sendstr_chunk(req, info);
            }
            header_checked = true;
        }

        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            free(buf);
            char msg[64];
            snprintf(msg, sizeof(msg), "ERR: write failed: %s\n", esp_err_to_name(err));
            httpd_resp_sendstr_chunk(req, msg);
            httpd_resp_send_chunk(req, NULL, 0);
            idle_timer_arm_5min();
            return ESP_FAIL;
        }
        remaining -= r;
        written += r;
        
        // Send progress every ~64KB
        if (written % (64*1024) < 4096) {
            char progress[64];
            snprintf(progress, sizeof(progress), "OK: wrote %zu / %zu\n", written, req->content_len);
            httpd_resp_sendstr_chunk(req, progress);
        }
    }
    free(buf);

    httpd_resp_sendstr_chunk(req, "OK: write complete\n");

    // esp_ota_end performs validation (magic, checksum, etc.)
    if ((err = esp_ota_end(handle)) != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s (image validation failed)", esp_err_to_name(err));
        char msg[96];
        snprintf(msg, sizeof(msg), "ERR: validation failed: %s\n", esp_err_to_name(err));
        httpd_resp_sendstr_chunk(req, msg);
        httpd_resp_send_chunk(req, NULL, 0);
        idle_timer_arm_5min();
        return ESP_FAIL;
    }
    httpd_resp_sendstr_chunk(req, "OK: validated\n");

    // Set boot partition (with rollback support if menuconfig enabled)
    if ((err = esp_ota_set_boot_partition(update_part)) != ESP_OK) {
        ESP_LOGE(TAG, "set_boot failed: %s", esp_err_to_name(err));
        httpd_resp_sendstr_chunk(req, "ERR: set_boot failed\n");
        httpd_resp_send_chunk(req, NULL, 0);
        idle_timer_arm_5min();
        return ESP_FAIL;
    }
    httpd_resp_sendstr_chunk(req, "OK: boot set\n");
    httpd_resp_sendstr_chunk(req, "OK: rebooting...\n");
    httpd_resp_send_chunk(req, NULL, 0);
    
    ESP_LOGI(TAG, "OTA update complete, rebooting");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}