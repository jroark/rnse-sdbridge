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

static esp_netif_t *sta, *ap;
static volatile int have_ip, wanted;
static volatile int ready_fd = -1;
static volatile unsigned departures;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (have_ip)
            printf("Wi-Fi: the network is gone\n");
        have_ip = 0;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        printf("Wi-Fi: a device joined\n");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        departures = departures + 1;
        printf("Wi-Fi: a device left\n");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        printf("Wi-Fi: joined, address " IPSTR ", gateway " IPSTR "\n", IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        have_ip = 1;
    }
}

static int try_address(uint32_t addr)
{
    struct sockaddr_in a;
    struct timeval tv = { 3, 0 };
    int s, one = 1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(CONFIG_AA_PHONE_PORT);
    a.sin_addr.s_addr = addr;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    /* (The timeout bounds the sends to a phone that has gone, too.) */
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    {
        /* Two seconds for the connection: an address nobody has would
         * hold the task for much longer. */
        int flags = fcntl(s, F_GETFL, 0), r, err = 0;
        socklen_t len = sizeof err;
        fd_set w;
        struct timeval wait = { 2, 0 };
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        r = connect(s, (struct sockaddr *)&a, sizeof a);
        if (r && errno == EINPROGRESS) {
            FD_ZERO(&w);
            FD_SET(s, &w);
            r = select(s + 1, NULL, &w, NULL, &wait) == 1 ? 0 : -1;
            if (r)
                err = ETIMEDOUT;
            else if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) || err)
                r = -1;
        } else if (r) {
            err = errno;
        }
        fcntl(s, F_SETFL, flags);
        if (r) {
            /* Said now and then: the head unit server may just not be started. */
            static unsigned tries;
            if (tries++ % 5 == 0)
                printf("no head unit server at %s port %d (%s)\n", inet_ntoa(a.sin_addr), CONFIG_AA_PHONE_PORT, strerror(err));
            close(s);
            return -1;
        }
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("connected to the phone's head unit server at %s port %d\n", inet_ntoa(a.sin_addr), CONFIG_AA_PHONE_PORT);
    return s;
}

static int try_phone(void)
{
    if (CONFIG_AA_PHONE_HOST[0]) {
        struct hostent *h = gethostbyname(CONFIG_AA_PHONE_HOST);
        uint32_t addr;
        if (!h || h->h_addrtype != AF_INET)
            return -1;
        memcpy(&addr, h->h_addr, sizeof addr);
        return try_address(addr);
    }
    if (ap) {
        /* Each device that has joined, at the address it was given. */
        wifi_sta_list_t list;
        esp_netif_pair_mac_ip_t pair[ESP_WIFI_MAX_CONN_NUM];
        int i, s;
        if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num <= 0)
            return -1;
        for (i = 0; i < list.num; i++)
            memcpy(pair[i].mac, list.sta[i].mac, 6);
        if (esp_netif_dhcps_get_clients_by_mac(ap, list.num, pair) != ESP_OK) {
            printf("no addresses for the devices that joined\n");
            return -1;
        }
        for (i = 0; i < list.num; i++)
            if (pair[i].ip.addr && (s = try_address(pair[i].ip.addr)) >= 0)
                return s;
        /* A phone that was here before this chip restarted may keep the
         * address it had without asking again, and is then in nobody's
         * table: the pool's first addresses, as they were given out. */
        for (i = 0; i < list.num; i++)
            if (!pair[i].ip.addr) {
                esp_netif_ip_info_t ip;
                uint32_t k;
                if (esp_netif_get_ip_info(ap, &ip) != ESP_OK)
                    return -1;
                for (k = 2; k <= 4; k++)
                    if ((s = try_address((ip.ip.addr & ip.netmask.addr) | htonl(k))) >= 0)
                        return s;
                break;
            }
        return -1;
    }
    {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.gw.addr)
            return -1;
        return try_address(ip.gw.addr);
    }
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

unsigned net_departures(void)
{
    return departures;
}

int net_start(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t c;
    esp_err_t r;
    int join = CONFIG_AA_WIFI_SSID[0] != 0;
    if (!join && !CONFIG_AA_AP_SSID[0])
        return 0;
    r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (join)
        sta = esp_netif_create_default_wifi_sta();
    else
        ap = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* Nothing of Wi-Fi's kept in the flash: a write there needs the
     * second core, which is the card's alone (main/sdcard.c). */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    memset(&c, 0, sizeof c);
    if (join) {
        strlcpy((char *)c.sta.ssid, CONFIG_AA_WIFI_SSID, sizeof c.sta.ssid);
        strlcpy((char *)c.sta.password, CONFIG_AA_WIFI_PASSWORD, sizeof c.sta.password);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &c));
    } else {
        strlcpy((char *)c.ap.ssid, CONFIG_AA_AP_SSID, sizeof c.ap.ssid);
        strlcpy((char *)c.ap.password, CONFIG_AA_AP_PASSWORD, sizeof c.ap.password);
        c.ap.channel = 6;
        c.ap.max_connection = 2;
        c.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &c));
        have_ip = 1;
    }
    /* No power saving: it adds up to a beacon interval to what the phone
     * sends. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, 4, NULL, 0);
    if (join)
        printf("Wi-Fi: joining \"%s\"\n", CONFIG_AA_WIFI_SSID);
    else
        printf("Wi-Fi: the network \"%s\" is up (WPA2), the bridge at 192.168.4.1\n", CONFIG_AA_AP_SSID);
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
