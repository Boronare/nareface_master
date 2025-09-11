#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_event.h>
#include "freertos/event_groups.h"
#include <esp_timer.h>
#include <nvs_flash.h>
#include "esp_netif.h"
#include "esp_mac.h"
#include <string.h>
#include "globals.h"
#include "status.hpp"

#ifndef NAREDEF_WIFI
#define NAREDEF_WIFI

// SoftAP defaults for provisioning UI
#define PROV_AP_SSID "nareface-setup"
#define PROV_AP_PASSWORD ""
#define PROV_AP_CHANNEL 6

static EventGroupHandle_t s_wifi_event_group;


/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
    static const int CONNECTED_BIT = BIT0;
    static const int ESPTOUCH_DONE_BIT = BIT1;
    static const int SCAN_DONE_BIT = BIT2;

    wifi_config_t wifi_config;
    static bool s_ap_running = false;

    // Simple ring buffer for last scan results
    static wifi_ap_record_t s_scan_records[32];
    static uint16_t s_scan_count = 0;
    uint8_t softap_manual_start = 0;
static void start_softap();
static void stop_softap();
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        start_softap();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
        globalStatus &= ~GLOBALSTAT_CONNECTED; // not connected
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    globalStatus |= GLOBALSTAT_CONNECTED; // connected
    if(softap_manual_start == 0) {
        stop_softap();
    }
    xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
    }
}
void connect_wifi() {
    //load ssid and password from NVS
    esp_err_t err = nvs_flash_init_partition("nvs");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("nvs"));
        err = nvs_flash_init_partition("nvs");
    }
    ESP_ERROR_CHECK(err);
    nvs_handle_t my_handle;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &my_handle));
    size_t ssid_len = 0;
    size_t password_len = 0;
    if(nvs_get_str(my_handle, "ssid", NULL, &ssid_len)==ESP_ERR_NVS_NOT_FOUND){
        ESP_LOGI("WIFI", "SSID not found in NVS, starting AP mode");
        start_softap();
    }else{
        nvs_get_str(my_handle, "password", NULL, &password_len);
        ESP_LOGI("WIFI", "Password Length : %u", password_len);
        char *ssid = (char *)malloc(32);
        char *password = (char *)malloc(64);
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "ssid", ssid, &ssid_len));
        nvs_get_str(my_handle, "password", password, &password_len);
        ESP_LOGI("WIFI", "SSID:%s", ssid);
        ESP_LOGI("WIFI", "PASSWORD:%s", password);
        memcpy(wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        free(ssid);
        free(password);
    }
    nvs_close(my_handle);

    ESP_LOGI("WIFI", "NVS closed");
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    ESP_LOGI("WIFI", "Default WiFi STA interface created");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_LOGI("WIFI", "WiFi initialized");

    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL) );

    ESP_LOGI("WIFI", "event handlers registered");
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_LOGI("WIFI", "WiFi started");
}

// Start a minimal SoftAP for provisioning UI concurrent with STA
static void start_softap() {
    if (s_ap_running) return;
    // Switch to APSTA mode
    globalStatus |= GLOBALSTAT_PROVISIONING;
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    // Create AP netif if needed
    static esp_netif_t* ap_netif = nullptr;
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, PROV_AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(PROV_AP_SSID);
    ap_cfg.ap.channel = PROV_AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    if (strlen(PROV_AP_PASSWORD) > 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        strncpy((char*)ap_cfg.ap.password, PROV_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    s_ap_running = true;
}

static void stop_softap() {
    if (!s_ap_running) return;
    s_ap_running = false;
    // Switch back to STA only mode
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    globalStatus &= ~GLOBALSTAT_PROVISIONING;
}

// Trigger a passive scan and cache results
static esp_err_t wifi_scan_now(uint16_t max_records = 32) {
    //if AP is running, disconnect STA mode
    if (s_ap_running) {
        ESP_ERROR_CHECK(esp_wifi_disconnect());
    }
    // Try to scan while connected: disable PS to reduce dropouts
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = true;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    // Short active dwell per channel to minimize impact while connected
    scan_cfg.scan_time.active.min = 30; // ms
    scan_cfg.scan_time.active.max = 60; // ms
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW("WIFI", "scan_start failed: %s", esp_err_to_name(err));
        // Fallback: try passive quick scan
        memset(&scan_cfg, 0, sizeof(scan_cfg));
        scan_cfg.show_hidden = true;
        scan_cfg.scan_type = WIFI_SCAN_TYPE_PASSIVE;
        scan_cfg.scan_time.passive = 60; // ms per channel
        err = esp_wifi_scan_start(&scan_cfg, true);
    }
    if (err != ESP_OK) {
        // Give up
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return err;
    }
    s_scan_count = max_records;
    err = esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_records);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return err;
}

// Accessors for HTTP layer
static inline uint16_t wifi_scan_count() { return s_scan_count; }
static inline const wifi_ap_record_t* wifi_scan_records() { return s_scan_records; }
static inline esp_err_t wifi_set_credentials_and_connect(const char* ssid, const char* pass) {
    bzero(&wifi_config, sizeof(wifi_config));
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, pass ? pass : "", sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK( esp_wifi_disconnect() );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    return esp_wifi_connect();
}

static inline esp_err_t wifi_save_credentials_to_nvs(const char* ssid, const char* pass) {
    esp_err_t err = nvs_flash_init_partition("nvs");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("nvs"));
        err = nvs_flash_init_partition("nvs");
    }
    if (err != ESP_OK) return err;
    nvs_handle_t my_handle = 0;
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(my_handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(my_handle, "password", pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    return err;
}

#endif // NAREDEF_WIFI