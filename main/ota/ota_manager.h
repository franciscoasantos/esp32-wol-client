#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>

#include "esp_websocket_client.h"

// Atualização de firmware pelo ar. O download roda numa task dedicada: o
// comando chega na task do esp_websocket_client, e baixar ~1 MB ali dentro
// derrubaria a conexão e estouraria o stack dela.

// Chamar cedo no app_main. Se a imagem em execução ainda está em
// PENDING_VERIFY (acabou de ser instalada por OTA), arma a janela de
// auto-teste: sem confirmação dentro dela, o dispositivo volta sozinho para o
// firmware anterior.
void ota_manager_boot_check(void);

// Confirma a imagem atual e cancela o rollback. Chamar apenas de um ponto que
// prove que a cadeia inteira funciona (WiFi + SNTP + TLS + HMAC + servidor).
void ota_manager_confirm_running_image(void);

// Dispara o download. Retorna false sem iniciar nada se já houver um OTA em
// curso, se a URL for inválida ou se a versão oferecida for a que já roda
// (a menos que force seja true). Em falha, *err_out recebe um motivo curto.
bool ota_manager_start(const char *url,
                       const char *version,
                       bool force,
                       esp_websocket_client_handle_t client,
                       const char **err_out);

bool ota_manager_is_running(void);

#endif
