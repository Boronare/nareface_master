idf_component_register(SRCS "spimaster.cpp"
                    REQUIRES
                        app_update
                        esp_http_server
                        esp_timer
                        esp_wifi
                        esp_netif
                        esp_event
                        esp_adc
                        nvs_flash
                        driver
                        mdns
                        lwip
                    INCLUDE_DIRS ".")

# Embed binary files directly
target_add_binary_data(__idf_main "jpeg_fallback_source.jpg" BINARY)
target_add_binary_data(__idf_main "index.html" TEXT)

set(CMAKE_CXX_STANDARD 17)  # C++17 표준 사용