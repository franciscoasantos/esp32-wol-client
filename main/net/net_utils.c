#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "mbedtls/md.h"

#include "config.h"
#include "net_utils.h"

static const char *TAG = "ESP_WOL_NET";

void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting WiFi...");
    vTaskDelay(5000 / portTICK_PERIOD_MS);
}

void sync_time(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    int retry = 0;
    while (now < 1000000000 && retry < 20)
    {
        time(&now);
        ESP_LOGI(TAG, "Waiting for time sync... (%d)", retry);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        retry++;
    }

    if (now > 1000000000)
    {
        ESP_LOGI(TAG, "Time synchronized: %lld", (long long)now);
    }
    else
    {
        ESP_LOGW(TAG, "Time sync failed, proceeding anyway");
    }
}

void make_hmac(const char *token, char *output)
{
    unsigned char hmac[32];

    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, (unsigned char *)SECRET, strlen(SECRET));
    mbedtls_md_hmac_update(&ctx, (unsigned char *)token, strlen(token));
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);

    for (int i = 0; i < 32; i++)
    {
        sprintf(output + i * 2, "%02x", hmac[i]);
    }

    output[64] = 0;
}

bool get_device_mac_string(char *output, int output_size)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read device MAC (err=%s)", esp_err_to_name(err));
        return false;
    }

    snprintf(output, output_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device MAC: %s", output);
    return true;
}

bool parse_mac_string(const char *input, uint8_t *mac)
{
    if (!input)
    {
        return false;
    }

    int values[6] = {0};
    int count = 0;
    if (strchr(input, ':'))
    {
        count = sscanf(input, "%02x:%02x:%02x:%02x:%02x:%02x",
                       &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    }
    else if (strchr(input, '-'))
    {
        count = sscanf(input, "%02x-%02x-%02x-%02x-%02x-%02x",
                       &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    }
    else
    {
        count = sscanf(input, "%02x%02x%02x%02x%02x%02x",
                       &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    }

    if (count != 6)
    {
        return false;
    }

    for (int i = 0; i < 6; i++)
    {
        mac[i] = (uint8_t)values[i];
    }

    return true;
}

bool send_wake_on_lan(const unsigned char *mac)
{
    unsigned char packet[102];
    memset(packet, 0xFF, 6);

    for (int i = 0; i < 16; i++)
    {
        memcpy(&packet[6 + i * 6], mac, 6);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return false;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    int sent = sendto(sock, packet, sizeof(packet), 0,
                      (struct sockaddr *)&addr, sizeof(addr));

    if (sent > 0)
    {
        ESP_LOGI(TAG, "Wake-on-LAN packet sent (%d bytes)", sent);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send Wake-on-LAN packet");
        close(sock);
        return false;
    }

    close(sock);
    return true;
}
