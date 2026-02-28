#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_websocket_client.h"

#include "config.h"
#include "ws_protocol.h"
#include "ws_frame_reassembly.h"
#include "ws_transport.h"

static const char *TAG = "ESP_WOL_WS";

static ws_frame_reassembly_t ws_rx;
static char ws_device_mac[18] = "00:00:00:00:00:00";

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)handler_args;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket Connected!");
        ws_frame_reassembly_reset(&ws_rx);
        ws_protocol_on_connected(client, ws_device_mac);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket Disconnected");
        ws_frame_reassembly_reset(&ws_rx);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code != 0x01)
        {
            break;
        }

        if (data->payload_len <= 0 || data->data_len <= 0)
        {
            ESP_LOGW(TAG, "Empty payload received");
            break;
        }

        if (data->payload_offset == 0)
        {
            if (!ws_frame_reassembly_begin(&ws_rx, data->payload_len))
            {
                ESP_LOGE(TAG, "Out of memory while reassembling payload");
                break;
            }
        }

        if (!ws_frame_reassembly_append(&ws_rx, data->payload_offset, data->data_ptr, data->data_len, data->payload_len))
        {
            ESP_LOGW(TAG, "Invalid fragment payload (offset=%d data_len=%d payload_len=%d)",
                     data->payload_offset, data->data_len, data->payload_len);
            ws_frame_reassembly_reset(&ws_rx);
            break;
        }

        if (ws_frame_reassembly_is_complete(&ws_rx))
        {
            ws_protocol_handle_complete_text(client, ws_frame_reassembly_data(&ws_rx));
            ws_frame_reassembly_reset(&ws_rx);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket Error");
        ws_frame_reassembly_reset(&ws_rx);
        break;
    }
}

static void websocket_task(void *arg)
{
    esp_websocket_client_config_t ws_cfg = {
        .uri = WS_URI,
        .disable_auto_reconnect = true,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, client);

    const int min_backoff_ms = 2000;
    const int max_backoff_ms = 30000;
    int reconnect_backoff_ms = min_backoff_ms;
    TickType_t last_attempt_tick = 0;
    bool was_connected = false;

    while (1)
    {
        bool is_connected = esp_websocket_client_is_connected(client);
        if (is_connected)
        {
            if (ws_protocol_should_force_reconnect())
            {
                ESP_LOGW(TAG, "Forcing reconnect to retry get_config with backoff");
                ws_protocol_clear_force_reconnect();
                esp_websocket_client_stop(client);
                was_connected = false;
                continue;
            }

            if (!was_connected)
            {
                ESP_LOGI(TAG, "WebSocket connection stable; reset reconnect backoff");
            }
            reconnect_backoff_ms = min_backoff_ms;
            was_connected = true;
        }
        else
        {
            TickType_t now = xTaskGetTickCount();
            if (last_attempt_tick == 0 || (now - last_attempt_tick) >= pdMS_TO_TICKS(reconnect_backoff_ms))
            {
                ESP_LOGW(TAG, "Connecting to WebSocket: %s (backoff=%dms)", WS_URI, reconnect_backoff_ms);
                esp_websocket_client_stop(client);
                esp_websocket_client_start(client);
                last_attempt_tick = now;
                reconnect_backoff_ms = reconnect_backoff_ms * 2;
                if (reconnect_backoff_ms > max_backoff_ms)
                {
                    reconnect_backoff_ms = max_backoff_ms;
                }
            }
            was_connected = false;
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void ws_transport_start(const char *device_mac)
{
    ws_frame_reassembly_init(&ws_rx);

    if (device_mac)
    {
        snprintf(ws_device_mac, sizeof(ws_device_mac), "%s", device_mac);
    }

    xTaskCreatePinnedToCore(websocket_task, "websocket", 12288, NULL, 5, NULL, 1);
}
