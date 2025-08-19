#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_event.h>
#include "freertos/event_groups.h"
#include <esp_timer.h>
#include <nvs_flash.h>
#include "esp_netif.h"
#include <esp_smartconfig.h>
#include "esp_mac.h"

#ifndef NAREDEF_WIFI
#define NAREDEF_WIFI

#define CONFIG_DEFAULT_SSID "ESP32"
#define CONFIG_DEFAULT_PASSWORD "12345678"

static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
   static const int CONNECTED_BIT = BIT0;
   static const int ESPTOUCH_DONE_BIT = BIT1;

   wifi_config_t wifi_config;

static void smartconfig_example_task(void * parm);
static void event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI("WIFI", "Connecting to AP...");
        //delay 5 seconds
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        //if not connected, start smartconfig
        if(!(xEventGroupGetBits(s_wifi_event_group) & CONNECTED_BIT)){
            ESP_LOGI("WIFI", "Starting SmartConfig...");
            xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
    xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
    ESP_LOGI("WIFI", "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
    ESP_LOGI("WIFI", "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
    ESP_LOGI("WIFI", "Got SSID and password");

    smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
    uint8_t ssid[33] = { 0 };
    uint8_t rvd_data[33] = { 0 };

    bzero(&wifi_config, sizeof(wifi_config_t));
    memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));

    #ifdef CONFIG_SET_MAC_ADDRESS_OF_TARGET_AP
            wifi_config.sta.bssid_set = evt->bssid_set;
            if (wifi_config.sta.bssid_set == true) {
                ESP_LOGI("WIFI", "Set MAC address of target AP: "MACSTR" ", MAC2STR(evt->bssid));
                memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
            }
    #endif

            memcpy(ssid, evt->ssid, sizeof(evt->ssid));
            ESP_LOGI("WIFI", "SSID:%s", ssid);
            if (evt->type == SC_TYPE_ESPTOUCH_V2) {
                ESP_ERROR_CHECK( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
                ESP_LOGI("WIFI", "RVD_DATA:");
                for (int i=0; i<33; i++) {
                    printf("%02x ", rvd_data[i]);
                }
                printf("\n");
            }

            ESP_ERROR_CHECK( esp_wifi_disconnect() );
            ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
            esp_wifi_connect();
        } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
            xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
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
        ESP_LOGI("WIFI", "SSID not found in NVS, using default SSID");
        memcpy(wifi_config.sta.ssid, CONFIG_DEFAULT_SSID, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, CONFIG_DEFAULT_PASSWORD, sizeof(wifi_config.sta.password));
    }else{
        nvs_get_str(my_handle, "password", NULL, &password_len);
        ESP_LOGI("WIFI","Password Length : %u", password_len);
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

    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );

    ESP_LOGI("WIFI", "event handlers registered");
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_LOGI("WIFI", "WiFi started");
    //wait for wifi to connect
    xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

static void smartconfig_example_task(void * parm)
{
    EventBits_t uxBits;
    ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH) );
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_smartconfig_start(&cfg) );
    while (1) {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
        if(uxBits & CONNECTED_BIT) {
            ESP_LOGI("WIFI", "WiFi Connected to ap");
            //save SSID and password to NVS
            esp_err_t err = nvs_flash_init_partition("nvs");
            if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                ESP_ERROR_CHECK(nvs_flash_erase_partition("nvs"));
                err = nvs_flash_init_partition("nvs");
            }
            ESP_ERROR_CHECK(err);
            nvs_handle_t my_handle;
            ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &my_handle));
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "ssid", (const char *)wifi_config.sta.ssid));
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "password", (const char *)wifi_config.sta.password));
            ESP_LOGI("WIFI", "SSID:%s", wifi_config.sta.ssid);
            ESP_LOGI("WIFI", "PASSWORD:%s", wifi_config.sta.password);
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            nvs_close(my_handle);
            ESP_LOGI("WIFI", "SSID and password saved to NVS");
            ESP_ERROR_CHECK( esp_smartconfig_stop() );
        }
        if(uxBits & ESPTOUCH_DONE_BIT) {
            ESP_LOGI("WIFI", "smartconfig over");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
        }
    }
}
#endif // NAREDEF_WIFI