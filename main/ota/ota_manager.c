#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#include "led_controller.h"
#include "ota_manager.h"

static const char *TAG = "ESP_WOL_OTA";

// Janela de auto-teste da imagem recém-instalada, dimensionada a partir de
// medições reais: boot → WiFi → SNTP → TLS → get_config leva ~11 s nos dois
// dispositivos. O pior caso legítimo é ~75 s, quando o WebSocket precisa de
// todo o backoff (2+4+8+16+30 s) antes de uma conexão que ainda daria certo.
// 120 s cobre isso com margem de 1,6× sem esticar a recuperação à toa.
//
// Encurtar mais arriscaria reverter um firmware bom que só demorou; alongar só
// aumenta o tempo sem controle remoto, já que a fita continua acesa (restaurada
// da NVS) durante toda a janela.
#define OTA_SELF_TEST_TIMEOUT_US (120ULL * 1000000ULL)

// Progresso a cada 5%: cada envio é um frame no mesmo WebSocket que carrega
// todo o resto, e a fita já disputa CPU com o download.
#define OTA_PROGRESS_STEP_PCT 5

#define OTA_URL_MAX 384
#define OTA_VERSION_MAX 32

typedef struct
{
    char url[OTA_URL_MAX];
    char version[OTA_VERSION_MAX];
    esp_websocket_client_handle_t client;
} ota_context_t;

static portMUX_TYPE ota_mux = portMUX_INITIALIZER_UNLOCKED;
static bool ota_running = false;

static esp_timer_handle_t self_test_timer = NULL;
static bool pending_verify = false;

// ---------------------------------------------------------------------------
// Rollback
// ---------------------------------------------------------------------------

static void self_test_timeout_cb(void *arg)
{
    (void)arg;

    // Não conseguimos falar com o servidor dentro da janela. Marcar a imagem
    // como inválida e reiniciar devolve o controle ao slot anterior — é o que
    // evita ter que ir buscar o ESP com um cabo.
    ESP_LOGE(TAG, "Self-test window expired without server contact; rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_manager_boot_check(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running)
    {
        return;
    }

    ESP_LOGI(TAG, "Running from partition '%s' (version=%s)",
             running->label, esp_app_get_description()->version);

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK)
    {
        return;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return;
    }

    pending_verify = true;

    const esp_timer_create_args_t args = {
        .callback = self_test_timeout_cb,
        .name = "ota_selftest",
    };

    if (esp_timer_create(&args, &self_test_timer) != ESP_OK ||
        esp_timer_start_once(self_test_timer, OTA_SELF_TEST_TIMEOUT_US) != ESP_OK)
    {
        // Sem timer não há como armar o rollback automático. Melhor reverter
        // agora, com a imagem antiga sabidamente boa, do que ficar preso numa
        // imagem nova que talvez nunca confirme.
        ESP_LOGE(TAG, "Failed to arm self-test timer; rolling back now");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }

    ESP_LOGW(TAG, "Image is pending verification; rollback armed for %llu s",
             OTA_SELF_TEST_TIMEOUT_US / 1000000ULL);
}

void ota_manager_confirm_running_image(void)
{
    if (!pending_verify)
    {
        return;
    }

    pending_verify = false;

    if (self_test_timer)
    {
        esp_timer_stop(self_test_timer);
        esp_timer_delete(self_test_timer);
        self_test_timer = NULL;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to confirm image: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Image confirmed; rollback cancelled");
}

// ---------------------------------------------------------------------------
// Relato de progresso
// ---------------------------------------------------------------------------

static void send_progress(esp_websocket_client_handle_t client, int pct)
{
    if (!client)
    {
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "{\"action\":\"ota_progress\",\"pct\":%d}", pct);
    esp_websocket_client_send_text(client, msg, strlen(msg), pdMS_TO_TICKS(1000));
}

static void send_result(esp_websocket_client_handle_t client, bool ok, const char *detail)
{
    if (!client)
    {
        return;
    }

    char msg[192];
    if (ok)
    {
        snprintf(msg, sizeof(msg), "{\"action\":\"ota_result\",\"status\":\"ok\"}");
    }
    else
    {
        snprintf(msg, sizeof(msg),
                 "{\"action\":\"ota_result\",\"status\":\"error\",\"error\":\"%s\"}",
                 detail ? detail : "unknown");
    }
    esp_websocket_client_send_text(client, msg, strlen(msg), pdMS_TO_TICKS(1000));
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

static void ota_release(ota_context_t *ctx)
{
    free(ctx);

    portENTER_CRITICAL(&ota_mux);
    ota_running = false;
    portEXIT_CRITICAL(&ota_mux);
}

static void ota_fail(ota_context_t *ctx, esp_https_ota_handle_t handle, const char *detail)
{
    if (handle)
    {
        esp_https_ota_abort(handle);
    }

    ESP_LOGE(TAG, "OTA failed: %s", detail);
    send_result(ctx->client, false, detail);

    ota_release(ctx);
    vTaskDelete(NULL);
}

static void ota_task(void *arg)
{
    ota_context_t *ctx = (ota_context_t *)arg;

    ESP_LOGI(TAG, "OTA starting (version=%s)", ctx->version);

    // Silencia a animação antes de baixar. 589 LEDs SK6812 custam ~23,6 ms de
    // refresh RMT sem DMA por frame e já saturam o core 1 disputando
    // interrupções com o WiFi (ver led_controller.c); somando a isso que
    // esp_ota_write suspende o outro core enquanto escreve na flash, deixar o
    // efeito rodando torna o download lento e instável.
    led_controller_set_effect(LED_EFFECT_NONE, NULL, NULL, 100);

    esp_http_client_config_t http_cfg = {
        .url = ctx->url,
        // Mesmo bundle de CAs que já valida o wss:// atrás da Cloudflare.
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || handle == NULL)
    {
        ota_fail(ctx, handle, esp_err_to_name(err));
        return;
    }

    const int image_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "Image size reported by server: %d bytes", image_size);

    send_progress(ctx->client, 0);
    int last_reported_pct = 0;

    while (1)
    {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            break;
        }

        // Sem Content-Length não dá para calcular percentual; o download segue
        // normalmente, só sem barra de progresso.
        if (image_size <= 0)
        {
            continue;
        }

        const int read = esp_https_ota_get_image_len_read(handle);
        const int pct = (int)(((int64_t)read * 100) / image_size);
        if (pct >= last_reported_pct + OTA_PROGRESS_STEP_PCT && pct < 100)
        {
            last_reported_pct = pct;
            send_progress(ctx->client, pct);
        }
    }

    if (err != ESP_OK)
    {
        ota_fail(ctx, handle, esp_err_to_name(err));
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle))
    {
        ota_fail(ctx, handle, "incomplete_download");
        return;
    }

    // esp_https_ota_finish valida a imagem (esp_image_verify confere o SHA-256
    // que o próprio IDF anexa ao binário) e libera o handle — não cabe abort
    // depois daqui.
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK)
    {
        ota_fail(ctx, NULL, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "OTA complete; restarting into the new image");
    send_progress(ctx->client, 100);
    send_result(ctx->client, true, NULL);

    // Um respiro para os dois frames saírem antes do reset.
    vTaskDelay(pdMS_TO_TICKS(500));

    free(ctx);
    esp_restart();
}

bool ota_manager_is_running(void)
{
    portENTER_CRITICAL(&ota_mux);
    const bool running = ota_running;
    portEXIT_CRITICAL(&ota_mux);
    return running;
}

bool ota_manager_start(const char *url,
                       const char *version,
                       bool force,
                       esp_websocket_client_handle_t client,
                       const char **err_out)
{
    const char *err = NULL;

    if (!url || url[0] == '\0' || strlen(url) >= OTA_URL_MAX)
    {
        err = "invalid_url";
    }
    else if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)
    {
        err = "invalid_url_scheme";
    }
    else if (!force && version && version[0] != '\0' &&
             strcmp(version, esp_app_get_description()->version) == 0)
    {
        err = "already_on_this_version";
    }

    if (err)
    {
        if (err_out)
        {
            *err_out = err;
        }
        return false;
    }

    portENTER_CRITICAL(&ota_mux);
    const bool already_running = ota_running;
    if (!already_running)
    {
        ota_running = true;
    }
    portEXIT_CRITICAL(&ota_mux);

    if (already_running)
    {
        if (err_out)
        {
            *err_out = "ota_already_running";
        }
        return false;
    }

    // A cJSON raiz do comando morre assim que o handler retorna, então a URL
    // precisa viajar copiada para a task.
    ota_context_t *ctx = calloc(1, sizeof(ota_context_t));
    if (!ctx)
    {
        portENTER_CRITICAL(&ota_mux);
        ota_running = false;
        portEXIT_CRITICAL(&ota_mux);

        if (err_out)
        {
            *err_out = "out_of_memory";
        }
        return false;
    }

    snprintf(ctx->url, sizeof(ctx->url), "%s", url);
    snprintf(ctx->version, sizeof(ctx->version), "%s", version ? version : "");
    ctx->client = client;

    // Core 0: led_task e websocket estão as duas pinadas no core 1 com
    // prioridade 5. Prioridade 4 mantém o WebSocket respondendo ping durante o
    // download.
    if (xTaskCreatePinnedToCore(ota_task, "ota", 8192, ctx, 4, NULL, 0) != pdPASS)
    {
        free(ctx);

        portENTER_CRITICAL(&ota_mux);
        ota_running = false;
        portEXIT_CRITICAL(&ota_mux);

        if (err_out)
        {
            *err_out = "task_create_failed";
        }
        return false;
    }

    return true;
}
