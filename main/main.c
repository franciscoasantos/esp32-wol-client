#include "nvs_flash.h"
#include "esp_log.h"
#include "net_utils.h"
#include "led_controller.h"
#include "ota_manager.h"
#include "ws_client.h"

static const char *TAG = "ESP_WOL_MAIN";

void app_main()
{
    // Primeira coisa do boot, antes de qualquer passo que possa falhar: se esta
    // imagem veio de um OTA e ainda não foi confirmada, arma o rollback. Assim
    // todo `return` prematuro daqui para baixo continua sendo recuperável sem
    // cabo — antes, uma falha de NVS abortava o app_main antes de armar o
    // rollback e deixava o dispositivo inerte para sempre.
    ota_manager_boot_check();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // Acontece quando a partição nvs muda de tamanho (foi o caso na migração
        // para a tabela A/B, que a encolheu de 24K para 16K) ou quando ela fica
        // sem páginas livres. Apagar custa a config de LED salva, que o servidor
        // reenvia no get_config logo depois — barato perto de um dispositivo que
        // não sobe.
        ESP_LOGW(TAG, "NVS inutilizável (%s); apagando e tentando de novo", esp_err_to_name(err));

        err = nvs_flash_erase();
        if (err == ESP_OK)
        {
            err = nvs_flash_init();
        }
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    // LED antes da rede: com a configuração e a cor na NVS a fita acende em
    // ~200 ms em vez de ficar apagada durante WiFi + SNTP + TLS + get_config.
    if (!led_controller_start())
    {
        ESP_LOGE(TAG, "Failed to start LED controller");
        return;
    }

    if (!led_controller_restore())
    {
        ESP_LOGI(TAG, "No LED state in NVS; waiting for server config");
    }

    wifi_init();

    char device_mac[18] = "00:00:00:00:00:00";
    if (!get_device_mac_string(device_mac, sizeof(device_mac)))
    {
        ESP_LOGE(TAG, "Unable to get device MAC");
        return;
    }

    sync_time();

    ws_client_start(device_mac);
}
