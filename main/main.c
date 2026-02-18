#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "mbedtls/md.h"
#include "esp_websocket_client.h"

#include "config.h"

static const char *TAG = "ESP_WOL_WS";

//////////////////////////////////////////////////////////
// SNTP TIME SYNC
//////////////////////////////////////////////////////////

void sync_time()
{
    ESP_LOGI(TAG, "Initializing SNTP");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Aguarda sincronização (timeout 10s)
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

//////////////////////////////////////////////////////////
// HMAC AUTH
//////////////////////////////////////////////////////////

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
        sprintf(output + i * 2, "%02x", hmac[i]);

    output[64] = 0;
}

//////////////////////////////////////////////////////////
// WAKE ON LAN
//////////////////////////////////////////////////////////

void send_wake_on_lan(const unsigned char *mac)
{
    unsigned char packet[102];

    // Preenche os primeiros 6 bytes com 0xFF
    memset(packet, 0xFF, 6);

    // Repete o MAC address 16 vezes
    for (int i = 0; i < 16; i++)
    {
        memcpy(&packet[6 + i * 6], mac, 6);
    }

    // Cria socket UDP
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return;
    }

    // Habilita broadcast
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Configura endereço de broadcast
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9); // Porta padrão Wake-on-LAN
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    // Envia o pacote mágico
    int sent = sendto(sock, packet, sizeof(packet), 0,
                      (struct sockaddr *)&addr, sizeof(addr));

    if (sent > 0)
    {
        ESP_LOGI(TAG, "Wake-on-LAN packet sent (%d bytes)", sent);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to send Wake-on-LAN packet");
    }

    close(sock);
}

//////////////////////////////////////////////////////////
// WIFI
//////////////////////////////////////////////////////////

void wifi_init()
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS},
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting WiFi...");
    vTaskDelay(5000 / portTICK_PERIOD_MS);
}

//////////////////////////////////////////////////////////
// JSON PARSER FOR MAC ADDRESS
//////////////////////////////////////////////////////////

bool parse_mac_from_json(const char *json, unsigned char *mac)
{
    // Procura pelo campo "mac" no JSON
    const char *mac_field = strstr(json, "\"mac\":");
    if (!mac_field)
    {
        ESP_LOGE(TAG, "MAC field not found in JSON");
        return false;
    }
    mac_field += 7; // strlen("\"mac\":\")

    // Pula espaços e aspas
    while (*mac_field == ' ' || *mac_field == '\"')
        mac_field++;

    // Parse do MAC address (formato: AA:BB:CC:DD:EE:FF ou AABBCCDDEEFF)
    int values[6];
    int count = 0;

    // Tenta formato com :
    if (strchr(mac_field, ':'))
    {
        count = sscanf(mac_field, "%02x:%02x:%02x:%02x:%02x:%02x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    }
    else if (strchr(mac_field, '-'))
    {
        count = sscanf(mac_field, "%02x-%02x-%02x-%02x-%02x-%02x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    }
    else
    {
        // Formato sem separador
        count = sscanf(mac_field, "%02x%02x%02x%02x%02x%02x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    }

    if (count != 6)
    {
        ESP_LOGE(TAG, "Failed to parse MAC address (count=%d)", count);
        return false;
    }

    for (int i = 0; i < 6; i++)
    {
        mac[i] = (unsigned char)values[i];
    }

    ESP_LOGI(TAG, "Parsed MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return true;
}

//////////////////////////////////////////////////////////
// WEBSOCKET EVENT HANDLER
//////////////////////////////////////////////////////////

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket Connected!");

        // Envia autenticação
        char token[64];
        sprintf(token, "esp32-%lld", time(NULL));

        char hmac[65];
        make_hmac(token, hmac);

        char auth[256];
        sprintf(auth, "{\"token\":\"%s\",\"hmac\":\"%s\"}", token, hmac);

        esp_websocket_client_send_text(data->client, auth, strlen(auth), portMAX_DELAY);
        ESP_LOGI(TAG, "Auth sent: %s", auth);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket Disconnected");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01)
        { // Text frame
            ESP_LOGI(TAG, "Received: %.*s", data->data_len, (char *)data->data_ptr);

            // Parse MAC address do JSON
            unsigned char mac[6];
            char json_buffer[512];
            int copy_len = data->data_len < sizeof(json_buffer) - 1 ? data->data_len : sizeof(json_buffer) - 1;
            memcpy(json_buffer, data->data_ptr, copy_len);
            json_buffer[copy_len] = 0;

            if (parse_mac_from_json(json_buffer, mac))
            {
                send_wake_on_lan(mac);

                // Envia confirmação
                char response[128];
                sprintf(response, "{\"status\":\"ok\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                esp_websocket_client_send_text(data->client, response, strlen(response), portMAX_DELAY);
            }
            else
            {
                // Envia erro
                const char *error = "{\"status\":\"error\",\"message\":\"Invalid MAC\"}";
                esp_websocket_client_send_text(data->client, error, strlen(error), portMAX_DELAY);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket Error");
        break;
    }
}

//////////////////////////////////////////////////////////
// WEBSOCKET TASK
//////////////////////////////////////////////////////////

void websocket_task(void *arg)
{
    esp_websocket_client_config_t ws_cfg = {
        .uri = WS_URI,
        .reconnect_timeout_ms = 5000,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, client);

    ESP_LOGI(TAG, "Connecting to WebSocket: %s", WS_URI);
    esp_websocket_client_start(client);

    // Mantém a task rodando
    while (1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

//////////////////////////////////////////////////////////
// MAIN
//////////////////////////////////////////////////////////

void app_main()
{
    nvs_flash_init();
    wifi_init();
    sync_time();
    xTaskCreate(websocket_task, "websocket", 12288, NULL, 5, NULL);
}
