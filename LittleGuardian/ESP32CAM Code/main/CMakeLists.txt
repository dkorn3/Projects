idf_component_register(SRCS "CNN.c" "main.c" "server.c" "camera.c" "ble.c"
                    INCLUDE_DIRS 
                        "."
                        "$ENV{IDF_PATH}/components/bt/host/nimble/nimble/porting/nimble/include"
                        "$ENV{IDF_PATH}/components/bt/host/nimble/nimble/porting/npl/freertos/include"
                        "$ENV{IDF_PATH}/components/bt/porting/npl/freertos/include"
                        "$ENV{IDF_PATH}/components/bt/host/nimble/port/include"
                    REQUIRES 
                        esp_wifi esp_event esp_netif nvs_flash 
                        esp_http_server esp_camera spi_flash bt mdns

                    EMBED_FILES "model_weights.bin")