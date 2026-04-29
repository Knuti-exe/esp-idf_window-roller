idf_component_register(SRCS "window_roller.c"
                    INCLUDE_DIRS "."
                    REQUIRES easy_connect esp_driver_gpio ota custom_mqtt
                    EMBED_TXTFILES "certs/ca_cert.pem")
