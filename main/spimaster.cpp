#include "esp_http_server.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "mdns.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "button.h"

#include "globals.h"
#include "wifi.hpp"
#include "status.hpp"

static const char* TAG      = "SPIMASTER";
static const char HTML_INDEX[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>nareface Wi‑Fi Setup</title>"
"<style>body{font-family:sans-serif;margin:12px}button,input,select{font:16px sans-serif;padding:6px}#aps{margin-top:8px}</style>"
"</head><body>"
"<h2>Wi‑Fi 설정</h2>"
"<button onclick=scan()>AP 스캔</button>"
"<div id=aps></div>"
"<form id=f onsubmit='return submitProv(event)'>"
"<p><label>SSID <select id=ssidSel name=ssid></select></label></p>"
"<p><label>패스워드 <input type=password name=pass id=pass></label></p>"
"<p><button type=submit>연결</button></p>"
"</form>"
"<script>"
"async function scan(){const j=window.__APS||[];const s=document.getElementById('ssidSel');s.innerHTML='';j.forEach(ap=>{const o=document.createElement('option');o.text=ap.ssid+' ('+ap.rssi+')';o.value=ap.ssid;s.add(o)});document.getElementById('aps').innerText='AP수: '+j.length;}"
"async function submitProv(e){e.preventDefault();const fd=new FormData(document.getElementById('f'));const body=new URLSearchParams(fd).toString();const r=await fetch('/provision',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});alert(r.ok?'저장됨. 연결 시도 중…':'실패');}"
"scan();""</script>"
"</body></html>";


uint8_t flags;
// Forward declaration for deep sleep helper used by timer task
static void enter_deep_sleep_wait_button();



void idle_timer_task(void* arg) {
    while(1){
        if(idle_counter > 0) idle_counter--;
        else enter_deep_sleep_wait_button();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
static inline void idle_timer_cancel() { idle_counter = 65535; }

static inline void idle_timer_arm_5min() { idle_counter = 30; }

// SPI handle
spi_device_handle_t spi_handle[3];

struct spijpeg_pack{
  uint16_t size;
  uint8_t bytes[4000];
};

constexpr int jpegbuf_size = sizeof(spijpeg_pack);

constexpr int target_fps = 33;
constexpr int target_frame_time = 30;

spijpeg_pack* spi_buffer;
SemaphoreHandle_t jpeg_semaphore;

// FPS calculation
uint8_t frame_count    = 0;
uint32_t last_fps_check = 0;
float    current_fps    = 0.0;


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
    lasttime[devnum] = current_time;
    if(elapsed_time < target_frame_time){
        vTaskDelay(pdMS_TO_TICKS(target_frame_time - elapsed_time));
        lasttime[devnum] += target_frame_time - elapsed_time;
    }
}

// HTTP streaming
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
static const char* _STREAM_BOUNDARY     = "\r\n--123456789000000000000987654321\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

// --- Provisioning HTTP handlers ---
static esp_err_t index_get_handler(httpd_req_t* req){
    // Arm idle sleep timer on non-stream request
    idle_timer_arm_5min();
    httpd_resp_set_type(req, "text/html");
    // Stream in chunks to reduce memory pressure
    // Prepend embedded scan results as window.__APS
    wifi_scan_now();
    const wifi_ap_record_t* recs = wifi_scan_records();
    uint16_t n = wifi_scan_count();
    httpd_resp_sendstr_chunk(req, "<script>window.__APS=");
    httpd_resp_sendstr_chunk(req, "[");
    for (uint16_t i=0;i<n;i++){
        char ssid[33]={0}; memcpy(ssid, recs[i].ssid, 32);
        char item[96];
        int len = snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}", i?",":"", ssid, recs[i].rssi);
        httpd_resp_send_chunk(req, item, len);
    }
    httpd_resp_sendstr_chunk(req, "];</script>");
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
    if (c>='A' && c<='F') return c-'A'+10;
    if (c>='a' && c<='f') return c-'a'+10;
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
    char ssid[33]={0}; char pass[65]={0};
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
    softap_manual_start = 0;
    wifi_save_credentials_to_nvs(ssid, pass);
    wifi_set_credentials_and_connect(ssid, pass);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

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
        ESP_LOGE(TAG, "PIN_BAT GPIO %d is not ADC-capable on this chip", (int)PIN_BAT);
        // Fallback to ADC_UNIT_1 CH0 to avoid crash, but will read 0
        s_adc_unit_id = ADC_UNIT_1;
        s_adc_channel = (adc_channel_t)0;
    }
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = s_adc_unit_id,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_unit));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12, // up to ~3.6V FS on pin (we're <1V due to divider)
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_unit, s_adc_channel, &chan_cfg));

    // Try to init calibration (optional)
    adc_cali_init_once(s_adc_unit_id, chan_cfg.atten);
    s_adc_inited = true;
}

static float read_battery_voltage() {
    ensure_adc_init();
    int raw = 0;
    if (adc_oneshot_read(s_adc_unit, s_adc_channel, &raw) != ESP_OK) {
        return 0.0f;
    }
    float v_adc = 0.0f;
    if (s_adc_cali_inited && s_adc_cali) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
            v_adc = mv / 1000.0f;
        }
    }
    if (v_adc <= 0.0f) {
        // Fallback approximation for 11 dB attenuation
        v_adc = (raw / 4095.0f) * 3.6f;
    }
    // Divider: BAT -- 330k -- ADC -- 100k -- GND, Vbat = Vadc * (330+100)/100 = *4.3
    return v_adc * 4.3f;
}

static esp_err_t status_get_handler(httpd_req_t* req){
    // Arm idle sleep timer on non-stream request
    idle_timer_arm_5min();
    httpd_resp_set_type(req, "application/json");
    float vbat = read_battery_voltage();
    // Rough SoC estimation from voltage (Li-ion approximation)
    float soc = 0.0f;
    if (vbat <= 3.4f) soc = 0;
    else if (vbat >= 4.2f) soc = 100;
    else soc = (vbat - 3.4f) / (4.2f - 3.4f) * 100.0f; // simple linear
    char buf[96];
    // output : vbat, soc, free(memory)
    int len = snprintf(buf, sizeof(buf), "{\"vbat\":%.3f,\"soc\":%.0f,\"free\":%lu}", vbat, soc, esp_get_free_heap_size());
    return httpd_resp_send(req, buf, len);
}

// --- Button handling ---
static void enter_deep_sleep_wait_button() {
    // ESP_LOGI(TAG, "Entering deep sleep. Hold BTN for 3s to wake.");
    // Configure wake on BTN low level
    for(int8_t i=0;i<100;i++){
        gpio_set_level(PIN_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PIN_BTN, 0));
    gpio_set_level(PIN_LED, 0);
    gpio_set_level(PIN_POW, 0);
    esp_deep_sleep_start();
}

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
        xSemaphoreTake(jpeg_semaphore, portMAX_DELAY);
        memset(&t, 0, sizeof(t));
        t.length    = jpegbuf_size * 8; // 4 bytes * 8 bits
        t.rx_buffer = spi_buffer;

        esp_err_t ret = spi_device_transmit(spi_handle[spi_num], &t);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive JPEG size");
            xSemaphoreGive(jpeg_semaphore);
            continue;
        }
        if(spi_buffer->size == 0){
            xSemaphoreGive(jpeg_semaphore);
            continue;
        }
        if(spi_buffer->size > jpegbuf_size) {
            ESP_LOGE(TAG, "JPEG size too large: %d", (int)spi_buffer->size);
            xSemaphoreGive(jpeg_semaphore);
            continue;
        }
        //validate if data is correct jpeg format
        if(spi_buffer->bytes[0] != 0xFF || spi_buffer->bytes[1] != 0xD8){
            ESP_LOGE(TAG, "JPEG data is not valid");
            xSemaphoreGive(jpeg_semaphore);
            continue;
        }


        char   part_buf[128];

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf(part_buf, 128, _STREAM_PART, spi_buffer->size, (int)(esp_timer_get_time()/1000000), (int)(esp_timer_get_time()%1000000));

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            //send jpeg data chunks by 64bytes separated
            size_t offset = 0;
            while (offset < spi_buffer->size) {
                size_t chunk_size = spi_buffer->size - offset;
                if (chunk_size > 512) {
                    chunk_size = 512;
                }
                res = httpd_resp_send_chunk(req, (char *)spi_buffer->bytes + offset, chunk_size);
                if (res != ESP_OK) {
                    break;
                }
                offset += chunk_size;
            }
        }
        xSemaphoreGive(jpeg_semaphore);
        if (res != ESP_OK) {
            break;
        }
        update_fps();
    }

    return res;
}
// Unified stream task argument
struct StreamTaskArg { httpd_req_t* req; uint8_t index; };

static void stream_task(void* arg) {
    StreamTaskArg* a = (StreamTaskArg*)arg;
    httpd_req_t* req = a->req;
    uint8_t idx = a->index;
    gpio_set_level(PIN_POW, 1);
    globalStatus |= GLOBALSTAT_STREAMING; // streaming
    spistream_handler(req, idx);
    httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    flags &= ~(1<<idx);
    if(!flags) {
        gpio_set_level(PIN_POW, 0);
        // Stream ended and no other streams active: arm 5-minute idle sleep
        idle_timer_arm_5min();
        globalStatus &= ~GLOBALSTAT_STREAMING; // not streaming
    }
    free(a);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler_common(httpd_req_t* req) {
    // Cancel idle sleep while streaming
    idle_timer_cancel();
    uint8_t idx = (uint8_t)(uintptr_t)req->user_ctx; // 0:left,1:right,2:face
    if (flags & (1<<idx)) {
        ESP_LOGI(TAG, "Stream %u already started", idx);
        return ESP_OK;
    }
    if (httpd_resp_set_type(req, _STREAM_CONTENT_TYPE) != ESP_OK) return ESP_FAIL;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "18");
    httpd_req_t* copy = NULL;
    httpd_req_async_handler_begin(req, &copy);
    if (!copy) {
        ESP_LOGE(TAG, "Failed to create copy of request");
        return ESP_FAIL;
    }
    StreamTaskArg* a = (StreamTaskArg*)malloc(sizeof(StreamTaskArg));
    if (!a) { httpd_req_async_handler_complete(copy); return ESP_FAIL; }
    a->req = copy; a->index = idx;
    if (xTaskCreate(stream_task, "stream_task", 3*1024, a, 5, NULL) != pdPASS) {
        httpd_req_async_handler_complete(copy); free(a); return ESP_FAIL;
    }
    flags |= 1<<idx;
    return ESP_OK;
}

extern "C" void app_main(void) {
    gpio_set_direction(PIN_POW, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);

    gpio_set_direction(PIN_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BTN, GPIO_PULLUP_ONLY);
    
    gpio_set_level(PIN_POW, 0); // Power off the camera

    // Log reset reason and, if woke from deep sleep via button, require 3s hold to proceed
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", (int)rr);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Wake from deep sleep: hold BTN for 3s to boot");
        int64_t start = esp_timer_get_time()/1000;
        while (gpio_get_level(PIN_BTN) == 0) {
            if ((esp_timer_get_time()/1000 - start) >= 3500) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if ((esp_timer_get_time()/1000 - start) < 3000) {
            ESP_LOGW(TAG, "Hold not long enough, back to deep sleep");
            enter_deep_sleep_wait_button();
        }
    }

    // Set default button actions
    {
        long_press_action = enter_deep_sleep_wait_button;
        triple_click_action = [](){
            ESP_LOGI(TAG, "Triple-click: starting SoftAP");
            softap_manual_start = 1;
            start_softap();
        };
    }
    ESP_LOGI(TAG, "Button configured: long-press to sleep, triple-click to SoftAP");
    // Start button handler
    xTaskCreate(ledStatusTask, "led_status_task", 1024, NULL, 5, NULL);
    xTaskCreate(idle_timer_task, "idle_timer_task", 1024, NULL, 5, NULL);

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
    spi_buffer = (spijpeg_pack*)heap_caps_malloc(jpegbuf_size, MALLOC_CAP_DMA);
    if (!spi_buffer) {
        ESP_LOGE(TAG, "Failed to allocate SPI buffer");
        return;
    }
    jpeg_semaphore = xSemaphoreCreateMutex();

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
    config.stack_size       = 3*1024; // avoid stack overflow in handlers
    config.lru_purge_enable = true;   // free idle sessions when sockets scarce
    config.send_wait_timeout = 20;    // wait for send buffer to reduce EAGAIN
    config.recv_wait_timeout = 10;

    httpd_handle_t stream_httpd = NULL;
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_uri_t stream_uri[6] = {
            {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL},
            {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL},
            {.uri = "/provision", .method = HTTP_POST, .handler = provision_post_handler, .user_ctx = NULL},
            {.uri = "/left", .method = HTTP_GET, .handler = stream_handler_common, .user_ctx = (void*)0},
            {.uri = "/right", .method = HTTP_GET, .handler = stream_handler_common, .user_ctx = (void*)1},
            {.uri = "/face", .method = HTTP_GET, .handler = stream_handler_common, .user_ctx = (void*)2}
        };
        httpd_register_uri_handler(stream_httpd, &stream_uri[0]);
        httpd_register_uri_handler(stream_httpd, &stream_uri[1]);
        httpd_register_uri_handler(stream_httpd, &stream_uri[2]);
        httpd_register_uri_handler(stream_httpd, &stream_uri[3]);
        httpd_register_uri_handler(stream_httpd, &stream_uri[4]);
        httpd_register_uri_handler(stream_httpd, &stream_uri[5]);
    // Arm initial idle sleep timer once server is up
    idle_timer_arm_5min();
    }
}