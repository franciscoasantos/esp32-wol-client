#include <stdio.h>
#include <time.h>

#include "esp_log.h"

#include "net_utils.h"
#include "ws_protocol.h"
#include "ws_protocol_internal.h"

static const char *TAG = "ESP_WOL_WSP";

void ws_protocol_on_connected(esp_websocket_client_handle_t client, const char *device_mac)
{
    char token[64];
    snprintf(token, sizeof(token), "esp32-%lld", (long long)time(NULL));

    char hmac[65];
    make_hmac(token, hmac);

    const char *mac = device_mac ? device_mac : "00:00:00:00:00:00";

    char auth[320];
    snprintf(auth, sizeof(auth), "{\"token\":\"%s\",\"hmac\":\"%s\",\"mac\":\"%s\"}", token, hmac, mac);

    ws_protocol_send_json(client, auth);
    ESP_LOGI(TAG, "Auth sent (mac=%s token=%s)", mac, token);

    ws_protocol_send_json(client, "{\"action\":\"get_config\"}");
    ESP_LOGI(TAG, "Requested server config with get_config");
}
