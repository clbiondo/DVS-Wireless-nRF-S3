#ifndef TONE_SYNTH_H
#define TONE_SYNTH_H

#include <stdint.h>
#include <stdbool.h>

/* ── Sintese de TOM DIRECIONAL PURO, sem NoiseMap/modo absoluto ──
   Motivacao: pesquisa sobre como o Phase DJ evita cair em "modo interno"
   revelou que ele transmite, via RCA, SOMENTE o tom de 1kHz (velocidade/
   direcao) - sem NoiseMap (a parte que codifica posicao absoluta na
   midia). Fonte: post de alguem identificado como o proprio criador do
   sinal de controle da Serato, no forum oficial deles: "It's known
   Serato reads a 1K tone for 'forward/backward' movement... The IP part
   is white noise as a map which transmits location data. Phase
   obviously can't transmit that." Sem NoiseMap, o Serato so' faz
   tracking RELATIVO (correlacao diferencial de velocidade/direcao) -
   nunca tenta correlacionar com uma posicao absoluta cataloga, e por
   isso nunca tem descontinuidade de posicao que derrube pro modo
   interno.

   Decisao adicional do projeto: o modo absoluto (APS) nunca teve funcao
   real aqui de qualquer forma - ele existiria pra permitir "pular" pra
   outro trecho da midia levantando uma agulha fisica e recolocando em
   outro ponto do disco de timecode, o que nao existe neste hardware
   (giroscopio, sem agulha, sem disco de timecode fisico). Remover a
   tentativa de correlacao de posicao nao perde nenhuma funcionalidade
   real - so remove um risco (o proprio bug do Felipe expos isso: mesmo
   nosso codigo correto, sem overflow, ainda tenta codificar um NoiseMap
   que nunca vai bater com nenhum catalogo real, e essa tentativa e' o
   que pode gerar saltos que derrubam o tracking ocasionalmente).

   Arquitetura: mesmo acumulador de fase e LUT de seno da versao
   completa (cv02_synth) - so remove a modulacao de amplitude por bit do
   LFSR/tabela. Amplitude fica so' na convergencia suave ativo/inativo
   (mesmo AMP_CONVERGE_FACTOR), sem alternar entre dois niveis. */

typedef struct {
    int64_t phase_fixed;
    float   amp_current;
    volatile float current_velocity;
    volatile uint32_t last_pkt_ms;
    float   vel_effective;
} tone_state_t;

void tone_state_init(tone_state_t *st);

/* Preenche buf (n amostras estereo) com o tom direcional puro. now_ms
   deve vir de um relogio monotonico (esp_timer). */
void tone_fill(tone_state_t *st, uint32_t *buf, int n, uint32_t now_ms);

#endif /* TONE_SYNTH_H */
