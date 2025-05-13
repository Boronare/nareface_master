idf_component_register(SRCS "spimaster.cpp"
                    PRIV_REQUIRES spi_flash
                    REQUIRES
                    "esp_http_server"
                    "esp_timer"
                    "esp_new_jpeg"
                    "esp_wifi"
                    "nvs_flash"
                    INCLUDE_DIRS ".")
set(CMAKE_CXX_STANDARD 17)  # C++17 표준 사용