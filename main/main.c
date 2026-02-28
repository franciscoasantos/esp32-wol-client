#include "nvs_flash.h"
#include "esp_log.h"
#include "net_utils.h"
#include "led_controller.h"
#include "ws_client.h"

static const char *TAG = "ESP_WOL_MAIN";

void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    wifi_init();

    char device_mac[18] = "00:00:00:00:00:00";
    if (!get_device_mac_string(device_mac, sizeof(device_mac)))
    {
        ESP_LOGE(TAG, "Unable to get device MAC");
        return;
    }

    sync_time();

    if (!led_controller_start())
    {
        ESP_LOGE(TAG, "Failed to start LED controller");
        return;
    }

    ws_client_start(device_mac);
}
