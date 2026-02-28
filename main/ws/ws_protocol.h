#ifndef WS_PROTOCOL_H
#define WS_PROTOCOL_H

#include <stdbool.h>

#include "esp_websocket_client.h"

void ws_protocol_on_connected(esp_websocket_client_handle_t client, const char *device_mac);
void ws_protocol_handle_complete_text(esp_websocket_client_handle_t client, const char *json_buffer);
bool ws_protocol_should_force_reconnect(void);
void ws_protocol_clear_force_reconnect(void);

#endif
