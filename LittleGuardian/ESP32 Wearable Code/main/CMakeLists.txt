idf_component_register(
    SRCS "main.cpp" "gatt_svr.cpp" "spo2_algorithm.cpp" "heartRate.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        nvs_flash
        driver
        bt
)