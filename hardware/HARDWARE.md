# Esquema elétrico e ligações — DVS Wireless nRF+S3

> **Nota de revisão**: este documento foi extraído diretamente do código-fonte do firmware atual (definições de pino em `main.c`, `dock_pairing.c` e nos overlays do Zephyr), garantindo que reflete exatamente a fiação em uso. Dois pontos específicos (modo de operação do PCM5102 e o tipo exato do LED do dock) estão marcados como **[CONFIRMAR]** — são detalhes de hardware que não aparecem no firmware e precisam ser verificados fisicamente antes de publicar.

---

## 1. Receptor (RX) — ESP32-S3

O RX usa dois barramentos SPI independentes (um por deck) para os módulos de rádio, e dois barramentos I2S independentes (um por deck) para os DACs de áudio.

### 1.1 Rádio — nRF24L01+ (Deck 1)

| Sinal | GPIO ESP32-S3 |
|---|---|
| SCK | GPIO12 |
| MOSI | GPIO11 |
| MISO | GPIO13 |
| CSN (Chip Select) | GPIO10 |
| CE (Chip Enable) | GPIO9 |
| IRQ | GPIO14 |
| VCC | 3.3V |
| GND | GND |

Barramento: `SPI2_HOST`.

> **Importante**: o nRF24L01+ funciona em 3.3V. **Não alimentar em 5V** — pode danificar o módulo permanentemente. Módulos "clone" muitas vezes têm regulagem de tensão de entrada ruim; alimentar direto do 3.3V regulado da placa ESP32-S3 (não do pino 5V/VUSB) é a opção mais confiável.

### 1.2 Rádio — nRF24L01+ (Deck 2)

| Sinal | GPIO ESP32-S3 |
|---|---|
| SCK | GPIO7 |
| MOSI | GPIO8 |
| MISO | GPIO15 |
| CSN | GPIO16 |
| CE | GPIO17 |
| IRQ | GPIO18 |
| VCC | 3.3V |
| GND | GND |

Barramento: `SPI3_HOST`.

### 1.3 DAC de áudio — PCM5102 (Deck 1)

| Sinal | GPIO ESP32-S3 |
|---|---|
| BCK (bit clock) | GPIO4 |
| LRCK (word select) | GPIO6 |
| DIN (dados) | GPIO5 |
| VIN | 5V (VUSB) |
| GND | GND |

### 1.4 DAC de áudio — PCM5102 (Deck 2)

| Sinal | GPIO ESP32-S3 |
|---|---|
| BCK | GPIO42 |
| LRCK | GPIO2 |
| DIN | GPIO1 |
| VIN | 5V (VUSB) |
| GND | GND |

**[CONFIRMAR]** — Pinos de modo do PCM5102 (`SCK`, `FLT`, `DEMP`, `XSMT`): a maioria dos módulos breakout PCM5102 já vem com esses pinos pré-configurados internamente (modo master, filtro normal, sem de-ênfase, sem mute). Verificar no módulo específico usado se algum desses pinos precisa de ligação manual antes de publicar o esquema definitivo.

### 1.5 Botões de pareamento / troca de formato

| Sinal | GPIO ESP32-S3 | Observação |
|---|---|---|
| Botão dock 1 | GPIO40 | Pull-up interno ativado; botão liga o pino ao GND quando pressionado |
| Botão dock 2 | GPIO41 | Idem |

- **Toque rápido**: alterna o formato de timecode (afeta os dois decks simultaneamente).
- **Pressão contínua de 3 segundos**: entra em modo de pareamento para o deck correspondente.

### 1.6 UART de pareamento (conector do dock)

Usada apenas durante o pareamento, para o RX enviar o comando `PAIR:<canal>:<endereço>` para o TX encaixado naquele dock específico.

| Dock | UART | RX (ESP32) | TX (ESP32) |
|---|---|---|---|
| Dock 1 | UART_NUM_1 | GPIO21 | GPIO47 |
| Dock 2 | UART_NUM_2 | GPIO38 | GPIO39 |

Baud rate: 115200, 8N1.

### 1.7 LED de status

| Sinal | GPIO ESP32-S3 |
|---|---|
| LED (dados) | GPIO48 |

**[CONFIRMAR]** — tipo exato do LED (WS2812 endereçável de um fio, ou LED RGB comum de 4 pinos com resistores limitadores). O firmware controla via RMT (`dock_led.c`), compatível com protocolo WS2812. Se for WS2812, verificar necessidade de resistor em série (330-470Ω) na linha de dados e capacitor de desacoplamento (100-1000µF) na alimentação do LED.

---

## 2. Transmissor (TX) — nRF52840 (Adafruit Feather nRF52840)

Um único firmware genérico é usado para qualquer TX — a identidade (canal e endereço de rádio) é gravada na memória não-volátil durante o pareamento, não no momento da compilação.

### 2.1 Giroscópio — LSM6DS3 (via SPI)

| Sinal | Pino nRF52840 |
|---|---|
| SCK | P0.22 |
| MOSI | P0.24 |
| MISO | P0.29 |
| CS (Chip Select) | P0.31 |
| IRQ / DRDY (Data Ready) | P1.00 |
| VCC | 3.3V |
| GND | GND |

### 2.2 UART de pareamento (para conector do dock)

| Sinal | Pino nRF52840 |
|---|---|
| TX | P0.06 |
| RX | P0.08 |

Só é usada fisicamente quando o TX está encaixado no dock (via conector/pogo pins) — durante o uso normal no toca-discos, esses pinos ficam desconectados.

### 2.3 LED de status

| Sinal | Pino nRF52840 |
|---|---|
| LED | P0.15 |

### 2.4 Alimentação

- Bateria LiPo conectada ao conector JST-PH 2 pinos nativo da placa Adafruit Feather nRF52840 — o circuito de carregamento (via USB) já vem integrado na própria placa, **não é necessário nenhum módulo de carregamento externo** (diferente de outros projetos DVS que usam um TP4056 separado).
- Recarrega conectando um cabo USB (micro-USB ou USB-C, dependendo da revisão da placa) diretamente na Feather.

---

## 3. Conector do dock (interface TX ↔ RX durante pareamento)

O dock precisa expor fisicamente, por deck:

| Sinal | Lado TX (nRF52840) | Lado RX (ESP32-S3, por dock) |
|---|---|---|
| UART TX→RX | P0.06 | GPIOxx (RX daquele dock) |
| UART RX→TX | P0.08 | GPIOxx (TX daquele dock) |
| GND | GND | GND |
| Botão de pareamento | — (lido do lado do RX) | GPIO40/41 |

**[CONFIRMAR]** — mecanismo físico de contato do dock (pogo pins, conector JST, contatos expostos) e se a alimentação da bateria do TX é usada também para operar o botão físico (ou se o botão fica isolado no corpo do dock, sem contato elétrico com o TX). Verificar antes de desenhar o diagrama mecânico do dock.

---

## 4. Diagrama de blocos (visão geral)

```
                         ┌─────────────────────────┐
   TX (Deck 1)           │      ESP32-S3 (RX)       │           TX (Deck 2)
   nRF52840 + LSM6DS3    │                          │      nRF52840 + LSM6DS3
   (no prato 1)          │  ┌────────┐  ┌────────┐  │           (no prato 2)
        )))\             │  │nRF24L01│  │nRF24L01│  │             )))\
   rádio 2.4GHz ────────>│  │ Deck1  │  │ Deck2  │  │<──────── rádio 2.4GHz
                         │  └────────┘  └────────┘  │
                         │  ┌────────┐  ┌────────┐  │
                         │  │PCM5102 │  │PCM5102 │  │
                         │  │ Deck1  │  │ Deck2  │  │
                         │  └───┬────┘  └───┬────┘  │
                         └──────┼───────────┼───────┘
                                │           │
                          Saída│Estéreo Saída│Estéreo
                          (Line)│      (Line)│
                                v           v
                         Placa de som do computador
                          (entradas de linha)
```

Durante o pareamento, cada TX é fisicamente encaixado no dock correspondente, estabelecendo contato UART temporário com o RX; fora disso, toda a comunicação é via rádio 2.4GHz (nRF24L01, Enhanced ShockBurst).
