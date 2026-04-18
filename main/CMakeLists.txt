idf_component_register(SRCS "yeelight_home_control.cpp" PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_driver_gpio lwip network_provisioning bt)
target_add_binary_data(${COMPONENT_LIB} "../PRIVATE.txt" TEXT)
