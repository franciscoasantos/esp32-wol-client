# ESP32 WoL Client - Wake-on-LAN + LED Strip via WebSocket

Sistema de controle remoto de Wake-on-LAN e fita LED RGB (WS2812B) ou RGBW (SK6812) baseado em ESP32 com conexão WebSocket para acesso através de servidor VPS.

## 📋 Descrição

Este projeto permite controlar dispositivos remotamente via Wake-on-LAN e também alterar a cor de uma fita LED RGB (WS2812B) ou RGBW (SK6812) utilizando um ESP32. O ESP32 estabelece uma conexão WebSocket persistente com um servidor VPS, permitindo acesso remoto mesmo quando está atrás de NAT/firewall, sem necessidade de configurar port forwarding no roteador.

### Como Funciona

1. **ESP32** conecta-se à rede WiFi local
2. Estabelece conexão WebSocket persistente com o **servidor VPS**
3. Servidor VPS envia mensagens JSON com comando de Wake-on-LAN ou comando de cor RGB
4. ESP32 processa o comando recebido
5. Executa Wake-on-LAN na LAN local ou altera a cor da fita LED

```
[Internet] ← → [VPS WebSocket] ← → [ESP32] ← → [Dispositivo na LAN]
```

## ✨ Funcionalidades

- ✅ Conexão WebSocket com reconexão automática e backoff exponencial
- ✅ Autenticação HMAC-SHA256 com timestamp e MAC do ESP32
- ✅ Solicitação automática de configuração via `{"action":"get_config"}` após autenticação
- ✅ Configuração dinâmica da fita LED pelo servidor (`ledPin`, `ledCount` e `ledType`)
- ✅ Wake-on-LAN via pacote mágico UDP
- ✅ Controle de cor RGB global para fita LED WS2812B (`r`, `g`, `b`)
- ✅ Suporte a fita SK6812 RGBW com controle do canal branco (`w`)
- ✅ Padrões estáticos: gradiente por stops e trechos com cores próprias, guardados como descrição (não como buffer de pixels)
- ✅ Transição suave entre cores (`fadeMs`), interpolada na tarefa de LED
- ✅ Configuração e última cor persistidas em NVS — a fita acende no boot sem esperar WiFi + TLS + `get_config`
- ✅ Correção de gamma 2.2 por LUT — compensa a resposta logarítmica do olho, então o `breathing` varia de forma perceptualmente linear
- ✅ Animação baseada em relógio (`esp_timer`) com orçamento de frame derivado do tamanho da fita — a mesma animação roda na mesma velocidade em fitas de 90 e de 589 LEDs
- ✅ Oito efeitos animados rodando no próprio firmware, com velocidade e intensidade ajustáveis — renderizados de forma não-bloqueante na tarefa de LED, sem depender de fluxo contínuo do servidor
- ✅ Atualização de firmware pelo ar (OTA) via `esp_https_ota`, com progresso reportado ao servidor e **rollback automático** caso a imagem nova não consiga falar com o servidor em 5 minutos
- ✅ Reassembly de payload WebSocket fragmentado
- ✅ Tratamento de JSON inválido, `ping/pong` e respostas de erro padronizadas

## 🛠️ Requisitos

### Hardware
- ESP32 (qualquer variante: ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)
- Cabo USB para programação
- Dispositivo alvo com suporte a Wake-on-LAN

### Software
- [ESP-IDF v5.0+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- Servidor VPS com IP público (para túnel reverso)
- Python 3 (para ferramentas ESP-IDF)

## 📦 Instalação

### 1. Clonar o Repositório

```bash
git clone <url-do-repositorio>
cd esp32-wol-client
```

### 2. Configurar o Projeto

Edite o arquivo [main/config.h](main/config.h) com suas credenciais:

```c
// WiFi Configuration
#define WIFI_SSID "sua-rede-wifi"
#define WIFI_PASS "sua-senha-wifi"

// WebSocket Server Configuration
#define WS_URI "ws://192.168.1.100:9001"

// Security
#define SECRET "sua-chave-secreta-aleatoria"
```

#### Parâmetros de Configuração

| Parâmetro | Descrição | Exemplo |
|-----------|-----------|---------|
| `WIFI_SSID` | Nome da rede WiFi | `"MinhaRede"` |
| `WIFI_PASS` | Senha da rede WiFi | `"senha123"` |
| `WS_URI` | URL do servidor WebSocket | `"ws://192.99.145.97:9001"` ou `"wss://seu-dominio.com/ws"` |
| `SECRET` | Chave secreta para HMAC (16+ caracteres) | `"9f2a1c7e8b4d5f9a"` |

> **Importante:** `ledPin`, `ledCount` e `ledType` não ficam fixos no firmware. Eles são recebidos do servidor via ação `config` após o `get_config`.

### 3. Compilar e Flashear

```bash
# Configurar o alvo (esp32, esp32s2, esp32s3, esp32c3, etc.)
idf.py set-target esp32

# Compilar o projeto
idf.py build

# Flashear no ESP32
idf.py -p COM3 flash monitor
```

> **Nota:** Substitua `COM3` pela porta serial correta (Windows) ou `/dev/ttyUSB0` (Linux/Mac)

#### Gravação por cabo: só a primeira vez

O projeto usa uma tabela de partições A/B (`partitions.csv`, dois slots de app +
`otadata`) para permitir atualização pela rede. **A tabela de partições é lida
pelo bootloader antes de qualquer código da aplicação, então trocá-la é a única
coisa que não dá para fazer por OTA** — daí a necessidade de uma última gravação
por cabo em cada dispositivo.

Se o ESP ainda estiver com o layout antigo (`partitions_singleapp`):

```bash
idf.py fullclean
idf.py build
idf.py -p COM3 flash monitor
```

A partição `nvs` encolhe de 24K para 16K na migração, e os dados antigos ficam
para trás — o que faz o `nvs_flash_init()` falhar com
`ESP_ERR_NVS_NO_FREE_PAGES`. O firmware trata isso sozinho: detecta o erro,
apaga a NVS e reinicializa. A fita perde a configuração salva
(`pin`/`count`/`type`/`pattern`), mas o servidor reenvia tudo no `get_config`
logo após o handshake — só demora alguns segundos a mais para acender nesse
primeiro boot.

Um `idf.py -p COM3 erase-flash` antes do `flash` também resolve, e deixa a flash
num estado limpo, mas não é necessário.

> **Antes de gravar, confira o tamanho real da flash:**
>
> ```bash
> esptool -p COM3 flash-id
> ```
>
> O `partitions.csv` deste repositório assume **4 MB**. Gravar uma tabela maior
> que a flash real produz um dispositivo que não dá boot. O arquivo traz um
> layout alternativo comentado para módulos de 2 MB — nele os slots caem para
> 960 KB, o que só cabe com o binário compilado em `-Os`.

Depois dessa gravação, o ciclo passa a ser: `idf.py build` → enviar o
`build/esp32-wol-client.bin` pela aba **Dispositivos → ESP32** do servidor →
clicar em **Atualizar**. Versione as releases com tags anotadas
(`git tag -a v1.1.0 -m "..."`): o IDF usa `git describe` como versão do firmware,
e sem tags ela sai como `2b66a78-dirty`, que o servidor não consegue comparar
entre builds.

## 🖥️ Configuração do Servidor WebSocket

O servidor WebSocket deve:
1. Aceitar conexões WebSocket do ESP32
2. Validar autenticação HMAC-SHA256
3. Receber o MAC do ESP32 no payload de autenticação
4. Responder ao `get_config` com os dados de LED
5. Enviar mensagens JSON de comando WoL ou comando de cor da fita LED

### Protocolo de Comunicação

#### 1. Autenticação (ESP32 → Servidor)
Após conectar, o ESP32 envia:
```json
{
  "token": "esp32-1707825600",
    "hmac": "a3f2b1e4c5d6...",
    "mac": "AA:BB:CC:DD:EE:FF",
    "version": "v1.1.0"
}
```

`version` é `esp_app_get_description()->version` — a versão da imagem em
execução, que o servidor usa para marcar o dispositivo como desatualizado.
Servidores antigos simplesmente ignoram o campo.

Em seguida, o ESP32 solicita a configuração dinâmica:

```json
{
    "action": "get_config"
}
```

#### 2. Configuração dinâmica de LED (Servidor → ESP32)
Resposta esperada para `get_config`:

```json
{
    "action": "config",
    "status": "ok",
    "ledCount": 30,
    "ledPin": 2,
    "ledType": "ws2812b" // ou "sk6812"
}
```

Valores aceitos para `ledType`:
- `ws2812b` (RGB, padrão)
- `sk6812` (RGBW, ativa canal branco)

Se o servidor ainda não tiver configuração pronta, pode responder:

```json
{
    "action": "config",
    "status": "error",
    "error": "config_incomplete"
}
```

Nesse caso, o cliente força reconexão com backoff e tenta novamente.

#### 3. Comando Wake-on-LAN (Servidor → ESP32)
O servidor envia mensagens JSON com `action` obrigatório:
```json
{
  "action": "wol",
  "mac": "A8:A1:59:98:61:0E"
}
```

Formatos de MAC suportados:
- `AA:BB:CC:DD:EE:FF` (com dois-pontos)
- `AA-BB-CC-DD-EE-FF` (com hífens)
- `AABBCCDDEEFF` (sem separadores)

#### 4. Comando LED RGB/RGBW (Servidor → ESP32)
Também é possível enviar comando para alterar a cor da fita LED.

Formato RGB (WS2812B ou SK6812 RGB):
```json
{
    "action": "led",
    "r": 0,
    "g": 255,
    "b": 128,
    "fadeMs": 600
}
```
`fadeMs` é opcional (0-60000): com valor maior que zero o firmware interpola da cor sólida atual até a nova ao longo desse tempo, no ritmo do orçamento de frame. Ausente ou `0` aplica na hora.

> A transição parte sempre da última cor **sólida**. Se um efeito estiver rodando, o que está na fita é o frame do efeito, então há um salto antes do fade começar.

Formato RGBW (apenas para SK6812 RGBW):
```json
{
    "action": "led",
    "r": 0,
    "g": 255,
    "b": 128,
    "w": 64
}
```
O campo `w` (white) é opcional e só tem efeito se a fita for SK6812 RGBW.

#### 4a. Padrões estáticos (Servidor → ESP32)

Gradiente — o firmware interpola entre os stops ao longo da fita:
```json
{
    "action": "gradient",
    "stops": [
        { "pos": 0,   "r": 255, "g": 80, "b": 0 },
        { "pos": 255, "r": 0,   "g": 40, "b": 255 }
    ],
    "fadeMs": 800
}
```
- `stops`: 2 a 8 itens, `pos` de `0` a `255` **em ordem crescente** (a busca por pixel assume isso), `w` opcional
- erros: `invalid_stops`, `stops_out_of_order`, `need_two_stops`

Trechos com cores próprias:
```json
{
    "action": "segments",
    "segments": [
        { "from": 0,   "to": 199, "r": 255, "g": 0, "b": 0 },
        { "from": 200, "to": 588, "r": 0,   "g": 0, "b": 255 }
    ]
}
```
- `segments`: 1 a 8 trechos, índices inclusivos; pixel fora de todos fica apagado
- erros: `invalid_segments`, `need_one_segment`

Os dois aceitam `fadeMs` e interrompem qualquer efeito ativo. O padrão é guardado como **descrição** (stops/trechos), não como buffer de pixels — numa fita de 589 LEDs isso é a diferença entre ~120 bytes e ~2,3 KB, e o crossfade só precisa avaliar os dois padrões por pixel.

O `config` pode trazer `lastPattern` no mesmo formato (`{"type":"solid|gradient|segments", ...}`). Quando vem, tem prioridade sobre `lastLedColor` — senão reconectar jogaria uma cor sólida por cima do gradiente que a NVS acabou de restaurar.

#### 4b. Comando de Efeito (Servidor → ESP32)
Ativa uma animação que roda **no próprio firmware** (o servidor envia apenas um comando):
```json
{
    "action": "effect",
    "effect": "fire",
    "r": 255,
    "g": 100,
    "b": 50,
    "speed": 70,
    "intensity": 80
}
```
- `effect`: um dos nomes da tabela abaixo, ou `none` (para interromper e voltar à última cor sólida). Nome desconhecido responde `unknown_effect` — antes era tratado como `none` com status `ok`, então um typo virava "parou sem erro nenhum"
- `r`/`g`/`b`: cor base opcional
- `speed`: `0-100`, escala o período da animação (50 = padrão, 0 = 4x mais lento, 100 = 4x mais rápido)
- `intensity`: `0-100`, significado por efeito

| Efeito | Descrição | Usa a cor base | Intensidade controla |
|---|---|---|---|
| `breathing` | Pulsa o brilho suavemente | sim | profundidade do pulso |
| `rainbow` | Espectro percorrendo a fita | não | — |
| `fade` | Fita inteira trocando de matiz | não | — |
| `fire` | Chama subindo, paleta própria | não | altura da chama |
| `comet` | Cabeça com cauda deslizando | sim | tamanho da cauda |
| `twinkle` | Pontos piscando ao acaso | sim | densidade de estrelas |
| `wave` | Duas senoides somadas | sim | número de cristas |
| `wipe` | Preenche a fita e recomeça | sim | suavidade da borda |

Só o `fire` guarda estado entre frames: um mapa de calor de 1 byte por LED, alocado junto com a fita (589 bytes na fita da sala). Os demais derivam tudo da fase e de hashes, sem buffer.
- A animação é renderizada de forma não-bloqueante na tarefa de LED; receber um comando `led` (cor sólida) também interrompe o efeito

#### 4c. Atualização de firmware — OTA (Servidor → ESP32)

```json
{
    "action": "ota",
    "url": "https://wol.exemplo.net/firmware/latest.bin?token=esp32-...&hmac=...",
    "version": "v1.1.0",
    "size": 962928,
    "sha256": "b95cea..."
}
```

O ESP32 responde imediatamente com
`{"status":"ok","action":"ota","state":"started"}` — o ACK confirma apenas o
**aceite** do comando. O download roda numa task própria (core 0, prioridade 4):
fazê-lo no handler travaria a task do `esp_websocket_client` e derrubaria a
conexão.

Enquanto baixa, o firmware:

1. Interrompe o efeito ativo. Numa fita de 589 LEDs SK6812 o refresh RMT (sem
   DMA no ESP32 clássico) custa ~23,6 ms por frame e satura o core 1 disputando
   interrupções com o WiFi — e `esp_ota_write` ainda suspende o outro core
   durante a escrita na flash
2. Emite `{"action":"ota_progress","pct":N}` a cada ~5%
3. Ao terminar, valida a imagem (`esp_https_ota_finish` confere o SHA-256 que o
   ESP-IDF anexa ao binário), envia
   `{"action":"ota_result","status":"ok"}` e reinicia

Recusas possíveis (`{"status":"error","action":"ota","message":"..."}`):

| Motivo | Significado |
|---|---|
| `missing url` | comando sem `url` |
| `invalid_url` / `invalid_url_scheme` | URL vazia, longa demais, ou fora de `http://`/`https://` |
| `already_on_this_version` | a versão oferecida é a que já roda — mande `"force": true` para reinstalar |
| `ota_already_running` | já há um download em andamento |
| `out_of_memory` / `task_create_failed` | sem recursos para iniciar |

##### Rollback automático

A imagem instalada dá boot em estado `PENDING_VERIFY` e **só vira definitiva
depois de o servidor responder ao `get_config`** — a prova de que WiFi, SNTP,
TLS, HMAC e servidor estão todos de pé. Se essa confirmação não vier em 5
minutos, o firmware chama `esp_ota_mark_app_invalid_rollback_and_reboot()` e o
dispositivo volta para a imagem anterior sozinho.

É essa rede de segurança que torna o OTA aceitável num ESP de difícil acesso: um
firmware que compila, dá boot, mas não consegue conectar não exige cabo para ser
desfeito. Vale exercitá-la de propósito uma vez (publicando um firmware com
`WS_URI` inválido) antes de confiar nela.

#### 5. Confirmação (ESP32 → Servidor)
O ESP32 responde com:
```json
{
  "status": "ok",
    "action": "wol",
    "targetMac": "A8:A1:59:98:61:0E"
}
```

Para comando LED:
```json
// Para WS2812B ou SK6812 RGB
{
    "status": "ok",
    "action": "led",
    "r": 0,
    "g": 255,
    "b": 128
}
// Para SK6812 RGBW
{
    "status": "ok",
    "action": "led",
    "r": 0,
    "g": 255,
    "b": 128,
    "w": 64
}
```

Para comando de efeito:
```json
{
    "status": "ok",
    "action": "effect",
    "effect": "breathing"
}
```

Ou em caso de erro:
```json
{
  "status": "error",
    "action": "wol",
    "message": "Invalid or missing mac"
}
```

Outros retornos de erro comuns:

```json
{"status":"error","message":"Missing action"}
```

```json
{"status":"error","action":"led","error":"invalid_rgb"}
```

Para keepalive:

```json
{"action":"ping"}
```

Resposta:

```json
{"status":"ok","action":"pong"}
```

## 📱 Uso

1. Garanta que o servidor WebSocket está rodando
2. O ESP32 conectará automaticamente ao ligar
3. Após autenticar, o ESP32 enviará `{"action":"get_config"}`
4. O servidor deve responder com `{"action":"config","status":"ok","ledCount":N,"ledPin":P,"ledType":"ws2812b|sk6812"}`
5. Depois disso, envie JSON de Wake-on-LAN (`"action":"wol"`), LED (`"action":"led","r":0,"g":255,"b":128"` ou com `"w":64` para SK6812 RGBW) ou efeito (`"action":"effect","effect":"breathing"`)
6. O ESP32 executará o comando recebido e retornará confirmação

## 🔧 Wake-on-LAN - Configuração do Dispositivo

Para que o dispositivo alvo responda ao Wake-on-LAN:

### 1. Habilitar na BIOS/UEFI
- Acesse a BIOS/UEFI do computador
- Procure por opções como:
  - "Wake on LAN"
  - "Power On by PCI-E Device"
  - "PME Event Wake Up"
- Habilite essas opções

### 2. Configurar no Sistema Operacional

**Windows:**
1. Gerenciador de Dispositivos → Adaptador de Rede → Propriedades
2. Aba "Gerenciamento de Energia"
3. Marcar "Permitir que este dispositivo acorde o computador"
4. Aba "Avançado" → Habilitar "Wake on Magic Packet"

**Linux:**
```bash
sudo ethtool -s eth0 wol g
```

### 3. Descobrir o MAC Address

**Windows:**
```cmd
ipconfig /all
```

**Linux/Mac:**
```bash
ip link show
# ou
ifconfig
```

## 🐛 Troubleshooting

### ESP32 não conecta ao WiFi
- Verificar SSID e senha em [config.h](main/config.h)
- Conferir se a rede é 2.4GHz (ESP32 não suporta 5GHz)
- Verificar logs: `idf.py monitor`

### ESP32 não conecta ao servidor WebSocket
- Verificar se a URL WebSocket (`WS_URI`) está correta em [config.h](main/config.h)
- Conferir se o servidor WebSocket está rodando
- Validar que o firewall permite conexões na porta configurada
- Verificar logs de conexão: `idf.py monitor`

### Autenticação falha no servidor
- Verificar se o `SECRET` é exatamente o mesmo no ESP32 e no servidor
- Confirmar que o tempo do ESP32 está sincronizado (SNTP)
- Verificar logs de autenticação no servidor

### Wake-on-LAN não funciona
- Verificar que o formato do MAC address no JSON está correto
- Confirmar que Wake-on-LAN está habilitado na BIOS do dispositivo alvo
- Dispositivo alvo deve estar conectado via cabo Ethernet (WiFi normalmente não suporta WoL)
- Dispositivo deve estar em sleep/hibernação, não desligado completamente na fonte
- Verificar logs do ESP32 para confirmar que o pacote foi enviado

### Atualização OTA falha

- `already_on_this_version`: o `git describe` gera a versão do firmware, e sem
  tags anotadas ela fica igual entre builds (`2b66a78-dirty`). Use
  `git tag -a v1.1.0` ou mande `"force": true`
- `ESP_ERR_OTA_PARTITION_CONFLICT` ou nada acontece: o dispositivo ainda está com
  a tabela `partitions_singleapp`. Precisa da gravação por cabo descrita em
  "Compilar e Flashear"
- Falha de TLS no download: o mesmo bundle de CAs do `wss://` é usado aqui, então
  se o WebSocket conecta o download também deveria. Confira se o SNTP sincronizou
  — certificado não valida com o relógio em 1970
- Falta de memória (`ESP_ERR_NO_MEM` / erro de mbedtls): o download abre uma
  segunda sessão TLS além da do WebSocket. Reduzir
  `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN`/`OUT_CONTENT_LEN` de 16384 para 4096
  economiza ~24 KB por sessão
- Dispositivo voltou para a versão anterior sozinho: é o rollback funcionando. A
  imagem nova subiu mas não conseguiu completar o `get_config` em 5 minutos —
  veja o `idf.py monitor` ou os logs do servidor para o motivo real

### ESP32 não recebe mensagens do servidor
- Verificar que a mensagem JSON está corretamente formatada
- Confirmar que o ESP32 está autenticado antes de enviar comandos
- Confirmar que a etapa `get_config` foi respondida com `action: "config"` e `status: "ok"`
- Verificar logs do WebSocket no servidor e no ESP32

## 📊 Monitoramento

Para visualizar os logs em tempo real:

```bash
idf.py monitor
```

**Logs importantes:**
- `WebSocket Connected!` - Conexão WebSocket estabelecida
- `Auth sent (mac=... token=...)` - Autenticação enviada ao servidor
- `Requested server config with get_config` - Solicitação de configuração dinâmica
- `Restored from NVS: pin=... count=... type=... color=yes` - estado recuperado antes da rede
- `LED config unchanged (pin=... count=...) - keeping strip alive` - reconexão sem recriar a fita
- `Color persisted to NVS (r=... g=... b=... w=...)` - cor gravada após 5 s parada
- `Server config applied successfully (ledCount=... ledPin=...)` - LED configurado via servidor
- `LED strip configured: pin=... count=... type=... frame=...ms (~N fps)` - orçamento de frame calculado a partir do tamanho da fita
- `Command received: ...` - Mensagem JSON recebida do servidor
- `Wake-on-LAN packet sent (102 bytes)` - Pacote WoL enviado
- `WebSocket Disconnected` - Reconectando automaticamente com backoff

## 🔒 Segurança

### Autenticação HMAC-SHA256

O sistema utiliza autenticação baseada em HMAC-SHA256 com timestamp para garantir segurança:

**Como funciona:**
1. **Sincronização de tempo (SNTP):** ESP32 sincroniza relógio com `pool.ntp.org` ao iniciar
2. **Geração do token:** Cria token único com timestamp atual: `esp32-{timestamp}`
3. **HMAC:** Gera hash HMAC-SHA256 do token usando `SECRET` compartilhado
4. **Envio:** Transmite `{"token":"esp32-1234567890","hmac":"abc123...","mac":"AA:BB:CC:DD:EE:FF"}`
5. **Validação no VPS:** Servidor recalcula HMAC e valida timestamp

**Por que SNTP é essencial:**
- ESP32 inicia com relógio em 1/1/1970 (epoch = 0)
- Sem SNTP, timestamps seriam inválidos e rejeitados pelo servidor
- Sincronização garante que ESP32 e VPS compartilham mesma referência de tempo
- Previne replay attacks através de validação de janela de tempo

**Proteções implementadas:**
- ✅ **Autenticação HMAC-SHA256:** Impede conexões não autorizadas
- ✅ **Secret compartilhado:** Apenas quem possui `SECRET` pode gerar HMAC válido
- ✅ **Timestamp validation:** Janela de ±5 minutos previne replay attacks
- ✅ **SNTP sync:** Garante precisão do timestamp
- ✅ **WebSocket:** Comunicação bidirecional persistente e eficiente

**Recomendações adicionais:**
- Usar WSS (WebSocket Secure) em produção
- Trocar o `SECRET` por valor aleatório forte (16+ caracteres)
- Implementar rate limiting no servidor WebSocket
- Adicionar autenticação de usuário na aplicação que envia comandos
- Registrar tentativas de autenticação falhadas para monitoramento
- Considerar uso de certificados TLS para WSS

## 📝 Estrutura do Projeto

```
esp32-wol-client/
├── main/
│   ├── main.c              # Bootstrap da aplicação
│   ├── config.h            # Configurações estáticas (WiFi, WS_URI, SECRET)
│   ├── net/
│   │   ├── net_utils.h
│   │   └── net_utils.c     # WiFi, SNTP, HMAC, MAC, WoL
│   ├── led/
│   │   ├── led_controller.h
│   │   └── led_controller.c # Queue/tarefa de LED, funil de saída (gamma), padrões, transições, efeitos e NVS
│   ├── ota/
│   │   ├── ota_manager.h
│   │   └── ota_manager.c    # Download em task própria, progresso e rollback
│   ├── ws/
│   │   ├── ws_client.h
│   │   ├── ws_client.c      # Fachada WS
│   │   ├── ws_transport.h
│   │   ├── ws_transport.c   # Conexão, eventos e backoff
│   │   ├── ws_protocol.h
│   │   ├── ws_protocol.c
│   │   ├── ws_protocol_auth.c
│   │   ├── ws_protocol_commands.c # Dispatch: wol, led, gradient, segments, effect, ota, config, ping
│   │   ├── ws_protocol_internal.h
│   │   ├── ws_frame_reassembly.h
│   │   └── ws_frame_reassembly.c # Reassembly de frames fragmentados
│   ├── idf_component.yml   # Dependências do projeto
│   └── CMakeLists.txt
├── managed_components/
│   ├── espressif__esp_websocket_client/
│   └── espressif__led_strip/
├── partitions.csv          # Tabela A/B para OTA (dois slots de app + otadata)
├── sdkconfig.defaults      # Partições, flash size, rollback e -Os (versionado)
├── CMakeLists.txt          # Configuração CMake do projeto
├── sdkconfig               # Configuração ESP-IDF
└── README.md               # Esta documentação
```

## 🤝 Contribuindo

Contribuições são bem-vindas! Sinta-se à vontade para:
- Reportar bugs
- Sugerir novas funcionalidades
- Enviar pull requests

## 📄 Licença

Este projeto é fornecido como está, sem garantias. Use por sua conta e risco.

## 🌟 Recursos Adicionais

- [Documentação ESP-IDF](https://docs.espressif.com/projects/esp-idf/)
- [Wake-on-LAN Protocol](https://en.wikipedia.org/wiki/Wake-on-LAN)
- [ESP32 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf)

---

**Desenvolvido com ESP-IDF** 🚀
