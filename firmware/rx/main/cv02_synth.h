#ifndef CV02_SYNTH_H
#define CV02_SYNTH_H

#include <stdint.h>
#include <stdbool.h>

/* Estado de sintese CV02 para UM deck. Cada deck tem sua propria instancia
   (sem variaveis globais compartilhadas), permitindo os 2 decks rodarem
   em tasks/nucleos separados sem interferencia. */
typedef struct {
    int64_t phase_fixed;      /* acumulador de fase Q?.16 com sinal */
    float   amp_current;
    volatile float current_velocity;  /* atualizado pela task de radio */
    volatile uint32_t last_pkt_ms;    /* atualizado pela task de radio */
    float   vel_effective;    /* estado do decaimento de link, so' usado dentro de fill_cv02 */
    float   vel_smoothed;     /* Suavizacao exponencial da velocidade,
                                  mesma tecnica do RPM_SMOOTHING do Felipe
                                  Alme (VEL_SMOOTHING_ALPHA em cv02_synth.c).
                                  TESTADA E REJEITADA em 26/jul: causava a
                                  musica "escapar" em comparacao real contra
                                  o Phase DJ (o filtro introduz atraso de
                                  fase real, mesmo "parecendo" mais estavel
                                  no scratch). Atualmente DESATIVADA de
                                  proposito (alpha=1.0 = bypass exato, nao
                                  e teste temporario esquecido) - mantida
                                  como campo/mecanismo caso um dia se decida
                                  reativar com um alpha mais leve. */
    uint8_t tc_format;  /* indice em TC_FORMATS[] - multi-formato carrier */
} cv02_state_t;

void cv02_state_init(cv02_state_t *st);

/* Chamar UMA VEZ no boot, antes de qualquer cv02_fill(). Valida a tabela
   const contra o LFSR gerado ao vivo (256 primeiros bits). Retorna false
   se o header cv02_table.h estiver desatualizado/incompativel. */
bool cv02_table_selfcheck(void);


/* Preenche buf (n amostras estereo, 32 bits: 16 bits L + 16 bits R) com o
   sinal CV02 atual. now_ms deve vir de um relogio monotonico (esp_timer). */
void cv02_fill(cv02_state_t *st, uint32_t *buf, int n, uint32_t now_ms);

#endif /* CV02_SYNTH_H */
