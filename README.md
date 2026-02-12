# ESP32 WoL Client - Wake-on-LAN via WebSocket

Sistema de controle remoto Wake-on-LAN baseado em ESP32 com conexão WebSocket para acesso através de servidor VPS.

## 📋 Descrição

Este projeto permite controlar dispositivos remotamente via Wake-on-LAN utilizando um ESP32. O ESP32 estabelece uma conexão WebSocket persistente com um servidor VPS, permitindo acesso remoto mesmo quando está atrás de NAT/firewall, sem necessidade de configurar port forwarding no roteador.

### Como Funciona

1. **ESP32** conecta-se à rede WiFi local
2. Estabelece conexão WebSocket persistente com o **servidor VPS**
3. Servidor VPS envia mensagens JSON contendo MAC address do dispositivo a ser acordado
4. ESP32 recebe a mensagem e transmite pacote mágico WoL via broadcast UDP
5. Dispositivo alvo na rede local é ligado via Wake-on-LAN

```
[Internet] ← → [VPS WebSocket] ← → [ESP32] ← → [Dispositivo na LAN]
```

## ✨ Funcionalidades

- ✅ Conexão WebSocket com reconexão automática
- ✅ Autenticação HMAC-SHA256 com timestamp
- ✅ Recebimento de MAC address dinâmico via JSON
- ✅ Wake-on-LAN via pacote mágico UDP
- ✅ Logs detalhados via ESP-IDF
- ✅ Suporte a múltiplos formatos de MAC address
- ✅ Confirmação de envio de pacotes WoL

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
3. Enviar mensagens JSON contendo o MAC address do dispositivo a ser acordado

### Protocolo de Comunicação

#### 1. Autenticação (ESP32 → Servidor)
Após conectar, o ESP32 envia:
```json
{
  "token": "esp32-1707825600",
  "hmac": "a3f2b1e4c5d6..."
}
```

#### 2. Comando Wake-on-LAN (Servidor → ESP32)
O servidor envia mensagens JSON com o MAC address:
```json
{
  "mac": "A8:A1:59:98:61:0E"
}
```

Formatos de MAC suportados:
- `AA:BB:CC:DD:EE:FF` (com dois-pontos)
- `AA-BB-CC-DD-EE-FF` (com hífens)
- `AABBCCDDEEFF` (sem separadores)

#### 3. Confirmação (ESP32 → Servidor)
O ESP32 responde com:
```json
{
  "status": "ok",
  "mac": "A8:A1:59:98:61:0E"
}
```

Ou em caso de erro:
```json
{
  "status": "error",
  "message": "Invalid MAC"
}
```

### Exemplo de Servidor Node.js

```javascript
const WebSocket = require('ws');
const crypto = require('crypto');

const SECRET = '9f2a1c7e8b4d5f9a';
const PORT = 9001;

const wss = new WebSocket.Server({ port: PORT });

wss.on('connection', (ws) => {
    console.log('ESP32 connected');
    let authenticated = false;
    
    ws.on('message', (message) => {
        const data = JSON.parse(message);
        
        if (!authenticated) {
            // Validar HMAC
            const hmac = crypto.createHmac('sha256', SECRET)
                .update(data.token)
                .digest('hex');
            
            if (hmac === data.hmac) {
                console.log('ESP32 authenticated!');
                authenticated = true;
                
                // Exemplo: enviar comando WoL após autenticação
                // ws.send(JSON.stringify({
                //     mac: "A8:A1:59:98:61:0E"
                // }));
            } else {
                console.log('Authentication failed');
                ws.close();
            }
        } else {
            // Processar confirmação do ESP32
            console.log('Response from ESP32:', data);
        }
    });
    
    ws.on('close', () => {
        console.log('ESP32 disconnected');
    });
});

console.log(`WebSocket server listening on port ${PORT}`);
```

### Exemplo de Servidor Python

```python
import asyncio
import json
import hashlib
import hmac
import websockets

SECRET = '9f2a1c7e8b4d5f9a'
PORT = 9001

async def handle_client(websocket, path):
    print("ESP32 connected")
    authenticated = False
    
    async for message in websocket:
        data = json.loads(message)
        
        if not authenticated:
            # Validar HMAC
            token = data['token']
            expected_hmac = hmac.new(
                SECRET.encode(),
                token.encode(),
                hashlib.sha256
            ).hexdigest()
            
            if expected_hmac == data['hmac']:
                print("ESP32 authenticated!")
                authenticated = True
                
                # Exemplo: enviar comando WoL
                # await websocket.send(json.dumps({
                #     "mac": "A8:A1:59:98:61:0E"
                # }))
            else:
                print("Authentication failed")
                await websocket.close()
        else:
            # Processar confirmação do ESP32
            print(f"Response from ESP32: {data}")

async def main():
    async with websockets.serve(handle_client, "0.0.0.0", PORT):
        print(f"WebSocket server listening on port {PORT}")
        await asyncio.Future()  # run forever

if __name__ == "__main__":
    asyncio.run(main())
```

## 📱 Uso

1. Garanta que o servidor WebSocket está rodando
2. O ESP32 conectará automaticamente ao ligar
3. Do servidor, envie mensagens JSON com o MAC address desejado
4. O ESP32 enviará o pacote Wake-on-LAN
5. O dispositivo alvo será ligado (se estiver configurado corretamente)

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
- Verificar logs do WebSocket no servidor e no ESP32

## 📊 Monitoramento

Para visualizar os logs em tempo real:

```bash
idf.py monitor
```

**Logs importantes:**
- `Connecting WiFi...` - Conectando ao WiFi
- `Connecting to WebSocket: ws://...` - Tentando conectar ao servidor WebSocket
- `WebSocket Connected!` - Conexão WebSocket estabelecida
- `Auth sent: ...` - Autenticação enviada ao servidor
- `Received: ...` - Mensagem JSON recebida do servidor
- `Parsed MAC: ...` - MAC address extraído com sucesso
- `Wake-on-LAN packet sent (102 bytes)` - Pacote WoL enviado
- `WebSocket Disconnected` - Reconectando automaticamente

## 🔒 Segurança

### Autenticação HMAC-SHA256

O sistema utiliza autenticação baseada em HMAC-SHA256 com timestamp para garantir segurança:

**Como funciona:**
1. **Sincronização de tempo (SNTP):** ESP32 sincroniza relógio com `pool.ntp.org` ao iniciar
2. **Geração do token:** Cria token único com timestamp atual: `esp32-{timestamp}`
3. **HMAC:** Gera hash HMAC-SHA256 do token usando `SECRET` compartilhado
4. **Envio:** Transmite `{"token":"esp32-1234567890","hmac":"abc123..."}`
5. **Validação no VPS:** Servidor recalcula HMAC e valida timestamp

**Por que SNTP é essencial:**
- ESP32 inicia com relógio em 1/1/1970 (epoch = 0)
- Sem SNTP, timestamps seriam inválidos e rejeitados pelo servidor
- Sincronização garante que ESP32 e VPS compartilham mesma referência de tempo
- Previne replay attacks através de validação de janela de tempo

**Exemplo de validação no servidor VPS:**

```javascript
const crypto = require('crypto');

function validateAuth(auth, secret) {
    // Recalcula HMAC
    const hmac = crypto.createHmac('sha256', secret)
        .update(auth.token)
        .digest('hex');
    
    // Valida HMAC
    if (hmac !== auth.hmac) {
        return false; // HMAC inválido
    }
    
    // Extrai timestamp
    const timestamp = parseInt(auth.token.split('-')[1]);
    const now = Math.floor(Date.now() / 1000);
    
    // Valida janela de tempo (±5 minutos)
    if (Math.abs(now - timestamp) > 300) {
        return false; // Timestamp muito antigo/futuro
    }
    
    return true; // Autenticado com sucesso
}
```

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
│   ├── main.c              # Código principal com WebSocket
│   ├── config.h            # Configurações (WiFi, WS_URI, SECRET)
│   ├── idf_component.yml   # Dependências do projeto
│   └── CMakeLists.txt
├── managed_components/
│   └── espressif__esp_websocket_client/  # Componente WebSocket
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
