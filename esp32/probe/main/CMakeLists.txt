set(src ${CMAKE_CURRENT_LIST_DIR}/../../../src)
idf_component_register(
    SRCS probe.c ${src}/crc.c
    INCLUDE_DIRS . ${src}
    PRIV_REQUIRES esp_driver_gpio esp_driver_pcnt esp_hw_support esp_timer hal soc)
