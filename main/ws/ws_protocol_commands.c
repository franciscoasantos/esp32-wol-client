#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "net_utils.h"
#include "led_controller.h"
#include "ota_manager.h"
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

// fadeMs opcional, compartilhado por led/gradient/segments.
static uint16_t parse_fade_ms(cJSON *root)
{
    cJSON *fade_json = cJSON_GetObjectItemCaseSensitive(root, "fadeMs");
    if (cJSON_IsNumber(fade_json) && fade_json->valuedouble > 0)
    {
        return (fade_json->valuedouble > 60000) ? 60000 : (uint16_t)fade_json->valueint;
    }
    return 0;
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

    // fadeMs opcional: 0 (ou ausente) aplica na hora. O seletor de cor manda 0
    // porque já envia uma cor a cada 140 ms; cenas e rampas mandam algo maior.
    if (!led_controller_enqueue_fade(&color, parse_fade_ms(root), 100))
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

// Lê r/g/b/w de um objeto qualquer (stop ou segmento). w é opcional.
static bool parse_color_fields(cJSON *obj, led_color_t *color)
{
    cJSON *r = cJSON_GetObjectItemCaseSensitive(obj, "r");
    cJSON *g = cJSON_GetObjectItemCaseSensitive(obj, "g");
    cJSON *b = cJSON_GetObjectItemCaseSensitive(obj, "b");
    cJSON *w = cJSON_GetObjectItemCaseSensitive(obj, "w");

    if (!ws_protocol_cjson_to_u8(r, &color->red) ||
        !ws_protocol_cjson_to_u8(g, &color->green) ||
        !ws_protocol_cjson_to_u8(b, &color->blue))
    {
        return false;
    }

    color->white = 0;
    ws_protocol_cjson_to_u8(w, &color->white);
    return true;
}

// Parsing de padrão compartilhado entre os comandos gradient/segments e o
// `lastPattern` que vem no config — assim reconectar não desfaz um gradiente.
// Devolvem NULL em sucesso ou o código de erro a ser ecoado.

static const char *parse_gradient_stops(cJSON *stops, led_pattern_t *out)
{
    if (!cJSON_IsArray(stops)) return "invalid_stops";

    memset(out, 0, sizeof(*out));
    out->type = LED_PATTERN_GRADIENT;

    cJSON *stop = NULL;
    cJSON_ArrayForEach(stop, stops)
    {
        if (out->stop_count >= LED_MAX_STOPS) break;

        led_stop_t entry = {0};
        cJSON *pos = cJSON_GetObjectItemCaseSensitive(stop, "pos");
        if (!ws_protocol_cjson_to_u8(pos, &entry.pos) || !parse_color_fields(stop, &entry.color))
        {
            return "invalid_stops";
        }

        // Stops precisam vir em ordem crescente: a busca por pixel assume isso.
        if (out->stop_count > 0 && entry.pos < out->stops[out->stop_count - 1].pos)
        {
            return "stops_out_of_order";
        }

        out->stops[out->stop_count++] = entry;
    }

    return (out->stop_count < 2) ? "need_two_stops" : NULL;
}

static const char *parse_segments_array(cJSON *segments, led_pattern_t *out)
{
    if (!cJSON_IsArray(segments)) return "invalid_segments";

    memset(out, 0, sizeof(*out));
    out->type = LED_PATTERN_SEGMENTS;

    cJSON *segment = NULL;
    cJSON_ArrayForEach(segment, segments)
    {
        if (out->segment_count >= LED_MAX_SEGMENTS) break;

        cJSON *from = cJSON_GetObjectItemCaseSensitive(segment, "from");
        cJSON *to = cJSON_GetObjectItemCaseSensitive(segment, "to");
        led_segment_t entry = {0};

        if (!cJSON_IsNumber(from) || !cJSON_IsNumber(to) ||
            from->valueint < 0 || to->valueint < from->valueint ||
            !parse_color_fields(segment, &entry.color))
        {
            return "invalid_segments";
        }

        entry.from = (uint16_t)from->valueint;
        entry.to = (uint16_t)to->valueint;
        out->segments[out->segment_count++] = entry;
    }

    return (out->segment_count == 0) ? "need_one_segment" : NULL;
}

// { "type": "solid|gradient|segments", ... } — usado pelo lastPattern do config.
static bool parse_pattern_object(cJSON *obj, led_pattern_t *out)
{
    if (!cJSON_IsObject(obj)) return false;

    cJSON *type = cJSON_GetObjectItemCaseSensitive(obj, "type");
    const char *name = (cJSON_IsString(type) && type->valuestring) ? type->valuestring : "solid";

    if (strcmp(name, "gradient") == 0)
    {
        return parse_gradient_stops(cJSON_GetObjectItemCaseSensitive(obj, "stops"), out) == NULL;
    }

    if (strcmp(name, "segments") == 0)
    {
        return parse_segments_array(cJSON_GetObjectItemCaseSensitive(obj, "segments"), out) == NULL;
    }

    memset(out, 0, sizeof(*out));
    out->type = LED_PATTERN_SOLID;
    cJSON *color = cJSON_GetObjectItemCaseSensitive(obj, "color");
    return parse_color_fields(cJSON_IsObject(color) ? color : obj, &out->solid);
}
// Gradiente por stops: posição 0-255 ao longo da fita. O firmware interpola
// entre eles, então o payload não cresce com o tamanho da fita.
static bool handle_gradient_command(cJSON *root, esp_websocket_client_handle_t client)
{
    if (!led_controller_is_configured())
    {
        ws_protocol_send_error(client, "gradient", "LED not configured");
        return false;
    }

    led_pattern_t pattern;
    const char *error = parse_gradient_stops(cJSON_GetObjectItemCaseSensitive(root, "stops"), &pattern);
    if (error)
    {
        ws_protocol_send_error(client, "gradient", error);
        return false;
    }

    if (!led_controller_set_pattern(&pattern, parse_fade_ms(root), 100))
    {
        ws_protocol_send_error(client, "gradient", "LED queue busy");
        return false;
    }

    char response[96];
    snprintf(response, sizeof(response),
             "{\"status\":\"ok\",\"action\":\"gradient\",\"stops\":%u}", pattern.stop_count);
    ws_protocol_send_json(client, response);
    return true;
}

// Trechos da fita com cores próprias. Pixel fora de qualquer trecho fica apagado.
static bool handle_segments_command(cJSON *root, esp_websocket_client_handle_t client)
{
    if (!led_controller_is_configured())
    {
        ws_protocol_send_error(client, "segments", "LED not configured");
        return false;
    }

    led_pattern_t pattern;
    const char *error = parse_segments_array(cJSON_GetObjectItemCaseSensitive(root, "segments"), &pattern);
    if (error)
    {
        ws_protocol_send_error(client, "segments", error);
        return false;
    }

    if (!led_controller_set_pattern(&pattern, parse_fade_ms(root), 100))
    {
        ws_protocol_send_error(client, "segments", "LED queue busy");
        return false;
    }

    char response[96];
    snprintf(response, sizeof(response),
             "{\"status\":\"ok\",\"action\":\"segments\",\"segments\":%u}", pattern.segment_count);
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

    static const struct { const char *name; led_effect_t value; } EFFECT_NAMES[] = {
        { "breathing", LED_EFFECT_BREATHING },
        { "rainbow",   LED_EFFECT_RAINBOW },
        { "fade",      LED_EFFECT_FADE },
        { "fire",      LED_EFFECT_FIRE },
        { "comet",     LED_EFFECT_COMET },
        { "twinkle",   LED_EFFECT_TWINKLE },
        { "wave",      LED_EFFECT_WAVE },
        { "wipe",      LED_EFFECT_WIPE },
    };

    led_effect_t effect = LED_EFFECT_NONE;
    // `resolved` aponta sempre para um literal desta tabela (ou "none"), nunca
    // para a string recebida: é o que garante que o eco no ACK não possa
    // injetar aspas e quebrar o JSON de resposta.
    const char *resolved = "none";
    bool known = (strcmp(effect_name, "none") == 0);

    for (size_t i = 0; i < sizeof(EFFECT_NAMES) / sizeof(EFFECT_NAMES[0]); i++)
    {
        if (strcmp(effect_name, EFFECT_NAMES[i].name) == 0)
        {
            effect = EFFECT_NAMES[i].value;
            resolved = EFFECT_NAMES[i].name;
            known = true;
            break;
        }
    }

    // Antes um nome desconhecido interrompia o efeito e respondia status "ok",
    // então um typo virava "parou de funcionar, sem erro nenhum".
    if (!known)
    {
        ws_protocol_send_error(client, "effect", "unknown_effect");
        return false;
    }

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

    // speed/intensity opcionais (0..100). Ausentes = padrão de cada efeito.
    led_effect_params_t params = { LED_PARAM_DEFAULT, LED_PARAM_DEFAULT };
    cJSON *speed_json = cJSON_GetObjectItemCaseSensitive(root, "speed");
    cJSON *intensity_json = cJSON_GetObjectItemCaseSensitive(root, "intensity");
    if (cJSON_IsNumber(speed_json) && speed_json->valueint >= 0 && speed_json->valueint <= 100)
    {
        params.speed = (uint8_t)speed_json->valueint;
    }
    if (cJSON_IsNumber(intensity_json) && intensity_json->valueint >= 0 && intensity_json->valueint <= 100)
    {
        params.intensity = (uint8_t)intensity_json->valueint;
    }

    if (!led_controller_set_effect(effect, base_ptr, &params, 100))
    {
        ws_protocol_send_error(client, "effect", "LED queue busy");
        return false;
    }

    char response[96];
    snprintf(response, sizeof(response),
             "{\"status\":\"ok\",\"action\":\"effect\",\"effect\":\"%s\"}", resolved);
    ws_protocol_send_json(client, response);
    return true;
}

static bool handle_ota_command(cJSON *root, esp_websocket_client_handle_t client)
{
    cJSON *url_json = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(url_json) || url_json->valuestring == NULL)
    {
        ws_protocol_send_error(client, "ota", "missing url");
        return false;
    }

    cJSON *version_json = cJSON_GetObjectItemCaseSensitive(root, "version");
    const char *version = (cJSON_IsString(version_json) && version_json->valuestring)
                              ? version_json->valuestring
                              : NULL;

    cJSON *force_json = cJSON_GetObjectItemCaseSensitive(root, "force");
    const bool force = cJSON_IsTrue(force_json);

    // Só valida e delega: este handler roda na task do esp_websocket_client, e
    // baixar ~1 MB aqui dentro travaria a conexão e estouraria o stack dela.
    const char *err = NULL;
    if (!ota_manager_start(url_json->valuestring, version, force, client, &err))
    {
        ESP_LOGW(TAG, "Refused OTA request: %s", err ? err : "unknown");
        ws_protocol_send_error(client, "ota", err ? err : "unknown");
        return false;
    }

    // ACK imediato: confirma o aceite do comando, não o fim do flash. O
    // progresso vem depois em ota_progress/ota_result.
    ws_protocol_send_json(client, "{\"status\":\"ok\",\"action\":\"ota\",\"state\":\"started\"}");
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

        // lastPattern tem prioridade: sem ele, reconectar com um gradiente na
        // fita jogaria uma cor sólida por cima do que a NVS acabou de restaurar.
        static led_pattern_t initial_pattern;
        cJSON *last_pattern_json = cJSON_GetObjectItemCaseSensitive(root, "lastPattern");
        bool pattern_applied = false;
        if (parse_pattern_object(last_pattern_json, &initial_pattern))
        {
            led_controller_set_pattern(&initial_pattern, 0, 100);
            pattern_applied = true;
        }

        // Se houver lastLedColor, já define a cor inicial
        cJSON *last_color_json = cJSON_GetObjectItemCaseSensitive(root, "lastLedColor");
        if (!pattern_applied && cJSON_IsObject(last_color_json))
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

        // O servidor respondeu e a config foi aplicada: WiFi, SNTP, TLS, HMAC e
        // servidor estão todos comprovadamente de pé. É o ponto certo para
        // confirmar uma imagem recém-instalada e cancelar o rollback armado.
        ota_manager_confirm_running_image();

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
    else if (strcmp(action, "gradient") == 0)
    {
        handle_gradient_command(root, client);
    }
    else if (strcmp(action, "segments") == 0)
    {
        handle_segments_command(root, client);
    }
    else if (strcmp(action, "ping") == 0)
    {
        ws_protocol_send_json(client, "{\"status\":\"ok\",\"action\":\"pong\"}");
    }
    else if (strcmp(action, "ota") == 0)
    {
        handle_ota_command(root, client);
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
