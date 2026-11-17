# The SD bridge for an RP2040-Zero; sdbridge/build.sh runs this with
# PICO_SDK_PATH and PICO_BOARD set.
cmake_minimum_required(VERSION 3.13)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
project(sdbridge C CXX ASM)
pico_sdk_init()

add_executable(sdbridge src/main.c src/card.c src/volume.c src/crc.c)
pico_generate_pio_header(sdbridge ${CMAKE_CURRENT_LIST_DIR}/src/sd.pio)
target_compile_options(sdbridge PRIVATE -Wall -Wextra -O2)
# The log may not hold up the data side: a terminal that is open and
# not reading would otherwise block a printf for half a second.
target_compile_definitions(sdbridge PRIVATE PICO_STDIO_USB_STDOUT_TIMEOUT_US=1000)
target_link_libraries(sdbridge pico_stdlib pico_multicore hardware_pio hardware_dma hardware_vreg)
pico_set_binary_type(sdbridge copy_to_ram)
pico_enable_stdio_usb(sdbridge 1)
pico_enable_stdio_uart(sdbridge 0)
# A .bin; sdbridge/uf2.py makes the file for the board's USB drive (the
# SDK would want picotool for that).
add_custom_command(TARGET sdbridge POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:sdbridge> sdbridge.bin)

# The phone on the desk (src/sdphone.c): the USB port as a host, the log
# on UART0 (GP0, 115200 baud).
add_executable(sdphone src/sdphone.c src/aap.c)
pico_generate_pio_header(sdphone ${CMAKE_CURRENT_LIST_DIR}/src/sd.pio OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/phone)
target_include_directories(sdphone PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_options(sdphone PRIVATE -Wall -Wextra -O2)
target_link_libraries(sdphone pico_stdlib hardware_pio tinyusb_host)
pico_enable_stdio_usb(sdphone 0)
pico_enable_stdio_uart(sdphone 1)
add_custom_command(TARGET sdphone POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:sdphone> sdphone.bin)
