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
- ✅ Efeitos animados rodando no próprio firmware (`breathing`, `rainbow`, `fade`) — renderizados de forma não-bloqueante na tarefa de LED, sem depender de fluxo contínuo do servidor
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
    "mac": "AA:BB:CC:DD:EE:FF"
}
```

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
    "b": 128
}
```

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

#### 4b. Comando de Efeito (Servidor → ESP32)
Ativa uma animação que roda **no próprio firmware** (o servidor envia apenas um comando):
```json
{
    "action": "effect",
    "effect": "breathing",
    "r": 255,
    "g": 100,
    "b": 50
}
```
- `effect`: `breathing`, `rainbow`, `fade` ou `none` (para interromper e voltar à última cor sólida)
- `r`/`g`/`b`: cor base opcional, usada por efeitos como `breathing`
- A animação é renderizada de forma não-bloqueante na tarefa de LED; receber um comando `led` (cor sólida) também interrompe o efeito

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
- `Server config applied successfully (ledCount=... ledPin=...)` - LED configurado via servidor
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
│   │   └── led_controller.c # Queue/tarefa de LED, aplicação de cor e efeitos (breathing/rainbow/fade)
│   ├── ws/
│   │   ├── ws_client.h
│   │   ├── ws_client.c      # Fachada WS
│   │   ├── ws_transport.h
│   │   ├── ws_transport.c   # Conexão, eventos e backoff
│   │   ├── ws_protocol.h
│   │   ├── ws_protocol.c
│   │   ├── ws_protocol_auth.c
│   │   ├── ws_protocol_commands.c # Dispatch de comandos: wol, led, effect, config, ping
│   │   ├── ws_protocol_internal.h
│   │   ├── ws_frame_reassembly.h
│   │   └── ws_frame_reassembly.c # Reassembly de frames fragmentados
│   ├── idf_component.yml   # Dependências do projeto
│   └── CMakeLists.txt
├── managed_components/
│   ├── espressif__esp_websocket_client/
│   └── espressif__led_strip/
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
