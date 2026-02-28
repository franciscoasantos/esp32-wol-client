#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "esp_websocket_client.h"

#include "ws_protocol.h"
#include "ws_protocol_internal.h"

static volatile bool ws_force_reconnect = false;

bool ws_protocol_cjson_to_u8(const cJSON *item, uint8_t *value)
{
    if (!cJSON_IsNumber(item))
    {
        return false;
    }

    if (item->valuedouble < 0 || item->valuedouble > 255)
    {
        return false;
    }

    int value_int = item->valueint;
    if (value_int < 0 || value_int > 255)
    {
        return false;
    }

    *value = (uint8_t)value_int;
    return true;
}

void ws_protocol_send_json(esp_websocket_client_handle_t client, const char *payload)
{
    if (!client || !payload)
    {
        return;
    }
    esp_websocket_client_send_text(client, payload, strlen(payload), portMAX_DELAY);
}

void ws_protocol_send_error(esp_websocket_client_handle_t client, const char *action, const char *message)
{
    char response[196];
    if (action)
    {
        snprintf(response, sizeof(response), "{\"status\":\"error\",\"action\":\"%s\",\"message\":\"%s\"}", action, message ? message : "invalid payload");
    }
    else
    {
        snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"%s\"}", message ? message : "invalid payload");
    }
    ws_protocol_send_json(client, response);
}

void ws_protocol_send_led_invalid_rgb(esp_websocket_client_handle_t client)
{
    ws_protocol_send_json(client, "{\"status\":\"error\",\"action\":\"led\",\"error\":\"invalid_rgb\"}");
}

void ws_protocol_request_force_reconnect(void)
{
    ws_force_reconnect = true;
}

bool ws_protocol_is_force_reconnect(void)
{
    return ws_force_reconnect;
}

bool ws_protocol_should_force_reconnect(void)
{
    return ws_protocol_is_force_reconnect();
}

void ws_protocol_clear_force_reconnect_internal(void)
{
    ws_force_reconnect = false;
}

void ws_protocol_clear_force_reconnect(void)
{
    ws_protocol_clear_force_reconnect_internal();
}
