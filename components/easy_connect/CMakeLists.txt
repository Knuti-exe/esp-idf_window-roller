idf_component_register(SRCS "easy_connect.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi wpa_supplicant nvs_flash)
