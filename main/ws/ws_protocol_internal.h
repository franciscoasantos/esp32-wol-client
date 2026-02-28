#ifndef WS_PROTOCOL_INTERNAL_H
#define WS_PROTOCOL_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_websocket_client.h"
#include "cJSON.h"

void ws_protocol_send_json(esp_websocket_client_handle_t client, const char *payload);
void ws_protocol_send_error(esp_websocket_client_handle_t client, const char *action, const char *message);
void ws_protocol_send_led_invalid_rgb(esp_websocket_client_handle_t client);
bool ws_protocol_cjson_to_u8(const cJSON *item, uint8_t *value);

void ws_protocol_request_force_reconnect(void);
bool ws_protocol_is_force_reconnect(void);
void ws_protocol_clear_force_reconnect_internal(void);

#endif
