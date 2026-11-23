idf_component_register(SRCS "main.c" "rf_capture.c" "detector.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_spi esp_driver_gpio esp_driver_parlio esp_driver_uart esp_timer
             esp_hw_support esp_wifi esp_event nvs_flash)
if(C5_MINIMAL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C5_MINIMAL=1)
endif()
if(C5_ACCEPTANCE_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C5_ACCEPTANCE_TEST=1)
endif()
if(C5_RF_FAILURE_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE C5_RF_FAILURE_TEST=1)
endif()
