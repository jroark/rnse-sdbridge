#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "net.h"

static esp_netif_t *sta;
static volatile int have_ip, wanted;
static volatile int ready_fd = -1;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (have_ip)
            printf("Wi-Fi: the network is gone\n");
        have_ip = 0;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        printf("Wi-Fi: joined, address " IPSTR ", gateway " IPSTR "\n", IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        have_ip = 1;
    }
}

static int try_phone(void)
{
    struct sockaddr_in a;
    struct timeval tv = { 3, 0 };
    int s, one = 1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(CONFIG_AA_PHONE_PORT);
    if (CONFIG_AA_PHONE_HOST[0]) {
        struct hostent *h = gethostbyname(CONFIG_AA_PHONE_HOST);
        if (!h || h->h_addrtype != AF_INET)
            return -1;
        memcpy(&a.sin_addr, h->h_addr, sizeof a.sin_addr);
    } else {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.gw.addr)
            return -1;
        a.sin_addr.s_addr = ip.gw.addr;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    /* (The timeout bounds the sends to a phone that has gone, too.) */
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) {
        close(s);
        return -1;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("connected to the phone's head unit server at %s port %d\n", inet_ntoa(a.sin_addr), CONFIG_AA_PHONE_PORT);
    return s;
}

static void net_task(void *arg)
{
    for (;;) {
        if (have_ip && wanted && ready_fd < 0) {
            int s = try_phone();
            if (s >= 0)
                ready_fd = s;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

int net_start(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t c;
    esp_err_t r;
    if (!CONFIG_AA_WIFI_SSID[0])
        return 0;
    r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    memset(&c, 0, sizeof c);
    strlcpy((char *)c.sta.ssid, CONFIG_AA_WIFI_SSID, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, CONFIG_AA_WIFI_PASSWORD, sizeof c.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &c));
    /* No power saving: it adds up to a beacon interval to what the phone
     * sends. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, 4, NULL, 0);
    printf("Wi-Fi: joining \"%s\"\n", CONFIG_AA_WIFI_SSID);
    return 1;
}

void net_want(int want)
{
    wanted = want;
}

int net_take(void)
{
    int s = ready_fd;
    if (s >= 0)
        ready_fd = -1;
    return s;
}
