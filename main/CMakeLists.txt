# Sem REQUIRES de propósito: o project.cmake só faz o `main` depender de todos
# os componentes do build enquanto o usuário não declara REQUIRES/PRIV_REQUIRES.
# Listar app_update/esp_https_ota aqui desligaria essa conveniência e obrigaria
# a enumerar também nvs_flash, esp_wifi, mbedtls e companhia.
idf_component_register(SRCS
                    "main.c"
                    "net/net_utils.c"
                    "led/led_controller.c"
                    "ota/ota_manager.c"
                    "ws/ws_client.c"
                    "ws/ws_transport.c"
                    "ws/ws_frame_reassembly.c"
                    "ws/ws_protocol.c"
                    "ws/ws_protocol_auth.c"
                    "ws/ws_protocol_commands.c"
                    INCLUDE_DIRS
                    "."
                    "net"
                    "led"
                    "ota"
                    "ws")
