#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "net_utils.h"
#include "led_controller.h"
#include "ws_protocol.h"
#include "ws_protocol_internal.h"

static const char *TAG = "ESP_WOL_WSP";

static bool parse_led_type(const cJSON *led_type_json, led_strip_type_t *led_type)
{
    if (!led_type)
    {
        return false;
    }

    if (!cJSON_IsString(led_type_json) || !led_type_json->valuestring)
    {
        *led_type = LED_STRIP_TYPE_WS2812B;
        return true;
    }

    if (strcmp(led_type_json->valuestring, "ws2812b") == 0)
    {
        *led_type = LED_STRIP_TYPE_WS2812B;
        return true;
    }

    if (strcmp(led_type_json->valuestring, "sk6812") == 0)
    {
        *led_type = LED_STRIP_TYPE_SK6812;
        return true;
    }

    return false;
}

static bool handle_wol_command(cJSON *root, esp_websocket_client_handle_t client)
{
    cJSON *mac_json = cJSON_GetObjectItemCaseSensitive(root, "mac");
    if (!cJSON_IsString(mac_json) || mac_json->valuestring == NULL)
    {
        ws_protocol_send_error(client, "wol", "Invalid or missing mac");
        return false;
    }

    uint8_t target_mac[6] = {0};
    if (!parse_mac_string(mac_json->valuestring, target_mac))
    {
        ws_protocol_send_error(client, "wol", "Invalid mac format");
        return false;
    }

    if (!send_wake_on_lan(target_mac))
    {
        ws_protocol_send_error(client, "wol", "Failed to send WoL packet");
        return false;
    }

    char response[160];
    snprintf(response, sizeof(response),
             "{\"status\":\"ok\",\"action\":\"wol\",\"targetMac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
             target_mac[0], target_mac[1], target_mac[2], target_mac[3], target_mac[4], target_mac[5]);
    ws_protocol_send_json(client, response);
    return true;
}

static bool handle_led_command(cJSON *root, esp_websocket_client_handle_t client)
{
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "r");
    cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "g");
    cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "b");
    cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "w");

    if (!led_controller_is_configured())
    {
        ws_protocol_send_error(client, "led", "LED not configured");
        return false;
    }

    led_color_t color = {0};
    if (!ws_protocol_cjson_to_u8(r, &color.red) ||
        !ws_protocol_cjson_to_u8(g, &color.green) ||
        !ws_protocol_cjson_to_u8(b, &color.blue))
    {
        ws_protocol_send_led_invalid_rgb(client);
        return false;
    }

    if (w && cJSON_IsNumber(w))
    {
        color.white = (uint8_t)w->valueint;
    }
    else
    {
        color.white = 0;
    }

    if (!led_controller_enqueue(&color, 100))
    {
        ws_protocol_send_error(client, "led", "LED queue busy");
        return false;
    }

    char response[160];
    if (w && cJSON_IsNumber(w))
    {
        snprintf(response, sizeof(response),
                 "{\"status\":\"ok\",\"action\":\"led\",\"r\":%u,\"g\":%u,\"b\":%u,\"w\":%u}",
                 color.red, color.green, color.blue, color.white);
    }
    else
    {
        snprintf(response, sizeof(response),
                 "{\"status\":\"ok\",\"action\":\"led\",\"r\":%u,\"g\":%u,\"b\":%u}",
                 color.red, color.green, color.blue);
    }
    ws_protocol_send_json(client, response);
    return true;
}

static bool handle_effect_command(cJSON *root, esp_websocket_client_handle_t client)
{
    if (!led_controller_is_configured())
    {
        ws_protocol_send_error(client, "effect", "LED not configured");
        return false;
    }

    cJSON *effect_json = cJSON_GetObjectItemCaseSensitive(root, "effect");
    const char *effect_name = (cJSON_IsString(effect_json) && effect_json->valuestring != NULL)
                                  ? effect_json->valuestring
                                  : "none";

    led_effect_t effect = LED_EFFECT_NONE;
    if (strcmp(effect_name, "breathing") == 0)
    {
        effect = LED_EFFECT_BREATHING;
    }
    else if (strcmp(effect_name, "rainbow") == 0)
    {
        effect = LED_EFFECT_RAINBOW;
    }
    else if (strcmp(effect_name, "fade") == 0)
    {
        effect = LED_EFFECT_FADE;
    }
    // "none" (ou desconhecido) mantém LED_EFFECT_NONE -> interrompe o efeito

    // Cor base opcional (usada por efeitos como breathing)
    led_color_t base = {0};
    led_color_t *base_ptr = NULL;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "r");
    cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "g");
    cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "b");
    if (ws_protocol_cjson_to_u8(r, &base.red) &&
        ws_protocol_cjson_to_u8(g, &base.green) &&
        ws_protocol_cjson_to_u8(b, &base.blue))
    {
        base_ptr = &base;
    }

    if (!led_controller_set_effect(effect, base_ptr, 100))
    {
        ws_protocol_send_error(client, "effect", "LED queue busy");
        return false;
    }

    char response[96];
    snprintf(response, sizeof(response),
             "{\"status\":\"ok\",\"action\":\"effect\",\"effect\":\"%s\"}", effect_name);
    ws_protocol_send_json(client, response);
    return true;
}

static bool handle_config_message(cJSON *root, esp_websocket_client_handle_t client)
{
    cJSON *status_json = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!cJSON_IsString(status_json) || status_json->valuestring == NULL)
    {
        ESP_LOGW(TAG, "Invalid config response: missing status");
        return false;
    }

    if (strcmp(status_json->valuestring, "ok") == 0)
    {
        cJSON *led_count_json = cJSON_GetObjectItemCaseSensitive(root, "ledCount");
        cJSON *led_pin_json = cJSON_GetObjectItemCaseSensitive(root, "ledPin");
        cJSON *led_type_json = cJSON_GetObjectItemCaseSensitive(root, "ledType");

        if (!cJSON_IsNumber(led_count_json) || !cJSON_IsNumber(led_pin_json))
        {
            ESP_LOGW(TAG, "Config response incomplete: missing ledCount or ledPin");
            ws_protocol_request_force_reconnect();
            return false;
        }

        int led_count = led_count_json->valueint;
        int led_pin = led_pin_json->valueint;
        led_strip_type_t led_type = LED_STRIP_TYPE_WS2812B;
        if (led_count <= 0 || led_pin < 0)
        {
            ESP_LOGW(TAG, "Config response invalid values (ledCount=%d ledPin=%d)", led_count, led_pin);
            ws_protocol_request_force_reconnect();
            return false;
        }

        if (!parse_led_type(led_type_json, &led_type))
        {
            const char *led_type_value = (cJSON_IsString(led_type_json) && led_type_json->valuestring) ? led_type_json->valuestring : "<missing>";
            ESP_LOGW(TAG, "Config response invalid ledType: %s", led_type_value);
            ws_protocol_request_force_reconnect();
            return false;
        }

        if (!led_controller_configure(led_pin, led_count, led_type))
        {
            ESP_LOGE(TAG, "Failed to apply server LED config");
            ws_protocol_request_force_reconnect();
            return false;
        }

        // Se houver lastLedColor, já define a cor inicial
        cJSON *last_color_json = cJSON_GetObjectItemCaseSensitive(root, "lastLedColor");
        if (cJSON_IsObject(last_color_json))
        {
            cJSON *r = cJSON_GetObjectItemCaseSensitive(last_color_json, "r");
            cJSON *g = cJSON_GetObjectItemCaseSensitive(last_color_json, "g");
            cJSON *b = cJSON_GetObjectItemCaseSensitive(last_color_json, "b");
            cJSON *w = cJSON_GetObjectItemCaseSensitive(last_color_json, "w");
            led_color_t color = {0};
            if (cJSON_IsNumber(r))
                color.red = (uint8_t)r->valueint;
            if (cJSON_IsNumber(g))
                color.green = (uint8_t)g->valueint;
            if (cJSON_IsNumber(b))
                color.blue = (uint8_t)b->valueint;
            if (cJSON_IsNumber(w))
                color.white = (uint8_t)w->valueint;
            led_controller_enqueue(&color, 100);
        }

        ESP_LOGI(TAG, "Server config applied successfully (ledCount=%d ledPin=%d ledType=%s)", led_count, led_pin, (led_type == LED_STRIP_TYPE_SK6812) ? "sk6812" : "ws2812b");

        // Reporta o estado atual da cor para o servidor
        led_color_t current_color = {0};
        cJSON *last_color_after = cJSON_GetObjectItemCaseSensitive(root, "lastLedColor");
        if (cJSON_IsObject(last_color_after))
        {
            cJSON *r2 = cJSON_GetObjectItemCaseSensitive(last_color_after, "r");
            cJSON *g2 = cJSON_GetObjectItemCaseSensitive(last_color_after, "g");
            cJSON *b2 = cJSON_GetObjectItemCaseSensitive(last_color_after, "b");
            if (cJSON_IsNumber(r2)) current_color.red   = (uint8_t)r2->valueint;
            if (cJSON_IsNumber(g2)) current_color.green = (uint8_t)g2->valueint;
            if (cJSON_IsNumber(b2)) current_color.blue  = (uint8_t)b2->valueint;
        }
        char state_report[128];
        snprintf(state_report, sizeof(state_report),
                 "{\"action\":\"state_report\",\"r\":%u,\"g\":%u,\"b\":%u,\"w\":%u}",
                 current_color.red, current_color.green, current_color.blue, current_color.white);
        ws_protocol_send_json(client, state_report);

        return true;
    }

    if (strcmp(status_json->valuestring, "error") == 0)
    {
        cJSON *error_json = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsString(error_json) && error_json->valuestring && strcmp(error_json->valuestring, "config_incomplete") == 0)
        {
            ESP_LOGW(TAG, "Server reported config_incomplete; reconnecting with backoff");
            ws_protocol_request_force_reconnect();
            return false;
        }

        if (cJSON_IsString(error_json) && error_json->valuestring)
        {
            ESP_LOGW(TAG, "Server returned config error: %s", error_json->valuestring);
        }
        else
        {
            ESP_LOGW(TAG, "Server returned unknown config error");
        }
        return false;
    }

    ESP_LOGW(TAG, "Unhandled config status: %s", status_json->valuestring);
    return false;
}

void ws_protocol_handle_complete_text(esp_websocket_client_handle_t client, const char *json_buffer)
{
    if (!json_buffer)
    {
        ws_protocol_send_error(client, NULL, "Invalid JSON payload");
        return;
    }

    ESP_LOGI(TAG, "Command received: %s", json_buffer);

    cJSON *root = cJSON_Parse(json_buffer);
    if (!root)
    {
        ESP_LOGE(TAG, "Invalid JSON payload");
        ws_protocol_send_error(client, NULL, "Invalid JSON payload");
        return;
    }

    cJSON *action_json = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action_json) || action_json->valuestring == NULL)
    {
        cJSON *error_json = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsString(error_json) && error_json->valuestring && strcmp(error_json->valuestring, "config_incomplete") == 0)
        {
            ESP_LOGW(TAG, "Received config_incomplete without action; forcing reconnect");
            ws_protocol_request_force_reconnect();
            cJSON_Delete(root);
            return;
        }

        ws_protocol_send_error(client, NULL, "Missing action");
        cJSON_Delete(root);
        return;
    }

    const char *action = action_json->valuestring;
    if (strcmp(action, "wol") == 0)
    {
        handle_wol_command(root, client);
    }
    else if (strcmp(action, "led") == 0)
    {
        handle_led_command(root, client);
    }
    else if (strcmp(action, "effect") == 0)
    {
        handle_effect_command(root, client);
    }
    else if (strcmp(action, "ping") == 0)
    {
        ws_protocol_send_json(client, "{\"status\":\"ok\",\"action\":\"pong\"}");
    }
    else if (strcmp(action, "config") == 0)
    {
        handle_config_message(root, client);
    }
    else
    {
        ws_protocol_send_error(client, action, "Unsupported action");
    }

    cJSON_Delete(root);
}
