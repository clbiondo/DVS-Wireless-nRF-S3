#ifndef NRF24_H
#define NRF24_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/spi_master.h"

/* Uma instancia por deck - permite ter 2 radios independentes (deck1/deck2)
   sem duplicar codigo, cada um com seu proprio barramento SPI e pinos. */
typedef struct {
    spi_host_device_t spi_host;
    spi_device_handle_t spi;
    int pin_sck;
    int pin_mosi;
    int pin_miso;
    int pin_csn;
    int pin_ce;
    int pin_irq;
    uint8_t rf_channel;
    uint8_t addr_bytes[5]; /* [0]=prefix, [1..4]=base_address (ver nota em nrf24.c) */
    const char *label;     /* "deck1" ou "deck2", so para logs */
} nrf24_dev_t;

/* dvs_packet identico ao struct do firmware Zephyr original (8 bytes) */
typedef struct {
    float angle;
    float velocity;
} __attribute__((packed)) dvs_packet_t;

/* Inicializa o radio: configura GPIOs, barramento SPI dedicado, registradores
   do nRF24L01 (canal, endereco, DPL, CRC16, 2Mbps). Fica em STANDBY (CE
   baixo) - chamar nrf24_enter_rx() so DEPOIS de armar a interrupcao de
   GPIO no chamador, para nao perder a borda de descida do IRQ. */
esp_err_t nrf24_init(nrf24_dev_t *dev);

/* Entra em modo RX de verdade (CE alto). Chamar SO depois que a interrupcao
   de GPIO ja estiver instalada (gpio_isr_handler_add) - ver nota em nrf24.c. */
void nrf24_enter_rx(nrf24_dev_t *dev);

/* Varredura de energia (CCA) num canal candidato - ver nota completa em
 * nrf24.c. Retorna true se detectou energia de RF (canal ocupado).
 * INTEGRACAO PENDENTE: ainda nao e chamada em nenhum lugar do fluxo de
 * pareamento - precisa coordenar com a task de radio ativa primeiro
 * (ver comentario em nrf24.c). */
bool nrf24_channel_busy(nrf24_dev_t *dev, uint8_t channel);

/* Le e limpa o flag RX_DR do registrador STATUS. Retorna true se havia
   dado pronto (chamar nrf24_read_packet em seguida). */
bool nrf24_data_ready(nrf24_dev_t *dev);

/* Le o payload do FIFO RX. Retorna true se o tamanho bateu com dvs_packet_t
   (8 bytes) e o pacote foi decodificado com sucesso em *out. */
bool nrf24_read_packet(nrf24_dev_t *dev, dvs_packet_t *out);

/* CORRECAO 26/jul: usar esta funcao em vez de nrf24_data_ready+nrf24_read_packet.
   Drena o FIFO RX inteiro (ate RX_EMPTY), aplicando so' o pacote mais
   recente em *out, e limpa RX_DR so' depois (ordem correta do datasheet).
   Corrige bug de acumulo de atraso de 2.4-4.8ms quando 2+ pacotes chegam
   entre um evento de IRQ e o outro (ver nota completa em nrf24.c).
   packets_drained (opcional, pode ser NULL): recebe quantos pacotes
   validos foram lidos nesta chamada - >1 indica que o FIFO acumulou,
   util pra medir a frequencia real do cenario em vez de estimar.
   Retorna true se pelo menos um pacote valido foi lido. */
bool nrf24_read_latest_packet(nrf24_dev_t *dev, dvs_packet_t *out, uint32_t *packets_drained);

#endif /* NRF24_H */
