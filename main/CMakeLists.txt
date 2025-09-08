idf_component_register(SRCS "spimaster.cpp"
                    REQUIRES
                    "esp_http_server"
                    "esp_timer"
                    "esp_wifi"
                    "esp_netif"
                    "esp_event"
                    "esp_adc"
                    "nvs_flash"
                    "driver"
                    "mdns"
                    INCLUDE_DIRS ".")
set(CMAKE_CXX_STANDARD 17)  # C++17 표준 사용