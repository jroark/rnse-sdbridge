set(src ${CMAKE_CURRENT_LIST_DIR}/../../../src)
idf_component_register(
    SRCS card_main.c ${src}/card.c ${src}/crc.c ${src}/volume.c
    INCLUDE_DIRS . ${src}
    PRIV_REQUIRES esp_driver_gpio esp_hw_support esp_timer hal soc)
# What answers a command runs from RAM, so that no read of the flash
# holds an answer up.
target_compile_options(${COMPONENT_LIB} PRIVATE -include esp_attr.h -DBRIDGE_RAM=IRAM_ATTR)
