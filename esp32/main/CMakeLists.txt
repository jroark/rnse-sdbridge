# The session is the bridge's own (../../src), the same files the RP2040
# and the host tests build.  AA_CERT_DIR: where esp32/build.sh put
# hu_cert.h (the head unit's certificate and key, never in the
# repository).
set(src ${CMAKE_CURRENT_LIST_DIR}/../../src)
idf_component_register(
    SRCS main.c usb_aoa.c net.c
         ${src}/aa_feed.c ${src}/aa_session.c ${src}/aa_tls.c ${src}/aa_hu.c ${src}/aap.c
    INCLUDE_DIRS . ${src} ${AA_CERT_DIR}
    PRIV_REQUIRES usb mbedtls esp_timer esp_wifi esp_netif nvs_flash lwip)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AAP_MESSAGE_MAX=8192 AA_TLS_OUT_BUF=4096)
