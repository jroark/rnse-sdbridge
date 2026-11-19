# The SD bridge for an RP2040-Zero; sdbridge/build.sh runs this with
# PICO_SDK_PATH and PICO_BOARD set.
cmake_minimum_required(VERSION 3.13)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
project(sdbridge C CXX ASM)
pico_sdk_init()

# The processor's clock for every firmware here (src/main.c, src/sdhandshake.c
# set it): sdbridge/build.sh SYS_KHZ=250000.  A variable, not CMAKE_C_FLAGS,
# which would replace the toolchain's processor flags.
if (NOT SYS_KHZ)
    set(SYS_KHZ 200000)
endif()
add_compile_definitions(SYS_KHZ=${SYS_KHZ})

add_executable(sdbridge src/main.c src/card.c src/volume.c src/crc.c src/status.c)
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
add_executable(sdphone src/sdphone.c src/aap.c src/aoa.c src/status.c)
pico_generate_pio_header(sdphone ${CMAKE_CURRENT_LIST_DIR}/src/sd.pio OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/phone)
target_include_directories(sdphone PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_options(sdphone PRIVATE -Wall -Wextra -O2)
target_link_libraries(sdphone pico_stdlib hardware_pio tinyusb_host)
pico_enable_stdio_usb(sdphone 0)
pico_enable_stdio_uart(sdphone 1)
add_custom_command(TARGET sdphone POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:sdphone> sdphone.bin)

# The handshake desk test (src/sdhandshake.c), only when the build has
# been given a certificate and key: AA_CERT_HEADER is the header
# sdbridge/pem2c.py made of them, under sdbridge/work/ (it holds the key).
if (NOT AA_MAX_UNACKED)
    set(AA_MAX_UNACKED 1)
endif()
if (AA_CERT_HEADER)
    get_filename_component(AA_CERT_DIR ${AA_CERT_HEADER} DIRECTORY)
    add_executable(sdhandshake src/sdhandshake.c src/aa_tls.c src/aa_hu.c src/aap.c src/aoa.c src/status.c)
    pico_generate_pio_header(sdhandshake ${CMAKE_CURRENT_LIST_DIR}/src/sd.pio OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/handshake)
    target_include_directories(sdhandshake PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src ${AA_CERT_DIR})
    target_compile_options(sdhandshake PRIVATE -Wall -Wextra -O2)
    target_link_libraries(sdhandshake pico_stdlib pico_multicore hardware_pio hardware_vreg tinyusb_host pico_mbedtls)
    pico_enable_stdio_usb(sdhandshake 0)
    pico_enable_stdio_uart(sdhandshake 1)
    add_custom_command(TARGET sdhandshake POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:sdhandshake> sdhandshake.bin)

    # The same with the session after it (src/aa_hu.c): the phone's
    # video, measured (SDSTREAM.UF2).  USB read 16 KiB at a time;
    # AA_MAX_UNACKED=N (sdbridge/build.sh) the frames the phone may send
    # ahead of the acknowledgements.
    add_executable(sdstream src/sdhandshake.c src/aa_tls.c src/aa_hu.c src/aap.c src/aoa.c src/status.c)
    pico_generate_pio_header(sdstream ${CMAKE_CURRENT_LIST_DIR}/src/sd.pio OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/stream)
    target_include_directories(sdstream PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src ${AA_CERT_DIR})
    target_compile_options(sdstream PRIVATE -Wall -Wextra -O2)
    target_compile_definitions(sdstream PRIVATE AA_STREAM=1 AOA_IN_SIZE=16384 AAP_MESSAGE_MAX=16384
        AA_MAX_UNACKED=${AA_MAX_UNACKED})
    target_link_libraries(sdstream pico_stdlib pico_multicore hardware_pio hardware_vreg tinyusb_host pico_mbedtls)
    pico_enable_stdio_usb(sdstream 0)
    pico_enable_stdio_uart(sdstream 1)
    add_custom_command(TARGET sdstream POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:sdstream> sdstream.bin)

    # The same with the USB's bulk transfers double-buffered (the TinyUSB
    # patch's PICO_USB_HOST_BULK_DOUBLE_BUFFER): SDSTREAMDB.UF2.
    add_executable(sdstreamdb src/sdhandshake.c src/aa_tls.c src/aa_hu.c src/aap.c src/aoa.c src/status.c)
    pico_generate_pio_header(sdstreamdb ${CMAKE_CURRENT_LIST_DIR}/src/sd.pio OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/streamdb)
    target_include_directories(sdstreamdb PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src ${AA_CERT_DIR})
    target_compile_options(sdstreamdb PRIVATE -Wall -Wextra -O2)
    target_compile_definitions(sdstreamdb PRIVATE AA_STREAM=1 AOA_IN_SIZE=16384 AAP_MESSAGE_MAX=16384
        AA_MAX_UNACKED=${AA_MAX_UNACKED} PICO_USB_HOST_BULK_DOUBLE_BUFFER=1)
    target_link_libraries(sdstreamdb pico_stdlib pico_multicore hardware_pio hardware_vreg tinyusb_host pico_mbedtls)
    pico_enable_stdio_usb(sdstreamdb 0)
    pico_enable_stdio_uart(sdstreamdb 1)
    add_custom_command(TARGET sdstreamdb POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:sdstreamdb> sdstreamdb.bin)
    # The bridge whole (src/sdlink.c): the phone's video into the slot,
    # SDLINK.UF2.  From flash (mbedTLS does not fit in RAM), the card's
    # side in RAM (BRIDGE_RAM, and no jump tables or library calls made
    # of its loops there); with smaller buffers than SDSTREAM's, for the
    # ring of blocks for the 192 (LINK_BLOCKS).  No default alarm pool:
    # its interrupt would be on core 0, the command line's.
    add_executable(sdlink src/sdlink.c src/card.c src/volume.c src/crc.c src/aastream.c
        src/aa_tls.c src/aa_hu.c src/aap.c src/aoa.c src/status.c)
    pico_generate_pio_header(sdlink ${CMAKE_CURRENT_LIST_DIR}/src/sd.pio OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/link)
    target_include_directories(sdlink PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src ${AA_CERT_DIR})
    target_compile_options(sdlink PRIVATE -Wall -Wextra -O2)
    # (Properties of the files, so SDBRIDGE's are built the same way.)
    set_source_files_properties(src/card.c src/volume.c src/crc.c src/aastream.c
        PROPERTIES COMPILE_OPTIONS "-fno-jump-tables;-fno-tree-loop-distribute-patterns")
    target_compile_definitions(sdlink PRIVATE AOA_IN_SIZE=8192 AAP_MESSAGE_MAX=8192
        AA_TLS_OUT_BUF=4096 MBEDTLS_SSL_OUT_CONTENT_LEN=4096 AA_MAX_UNACKED=${AA_MAX_UNACKED} VOLUME_PROGRAM_ROOM=512
        "BRIDGE_RAM=__attribute__((section(\".time_critical.bridge\")))"
        PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1)
    target_link_libraries(sdlink pico_stdlib pico_multicore hardware_pio hardware_dma hardware_vreg tinyusb_host pico_mbedtls)
    pico_enable_stdio_usb(sdlink 0)
    pico_enable_stdio_uart(sdlink 1)
    add_custom_command(TARGET sdlink POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:sdlink> sdlink.bin)
endif()
