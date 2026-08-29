/* cv02_synth.c - Sintese do sinal CV02, portada 1:1 da logica ja validada
 * no RX Zephyr (dvs_rx_deck*_main_25jul2026_freqcal.c). Mesma matematica,
 * mesmas constantes calibradas empiricamente (CV02_RESOLUTION_HZ=1020.4f,
 * ja recalibrada no osciloscopio nRF52840 - reconferir se o clock I2S real
 * do ESP32-S3 divergir o suficiente para precisar de outra recalibracao).
 *
 * Diferenca da versao Zephyr: estado em struct (cv02_state_t) por
 * instancia, em vez de globais static - permite 2 decks rodando em
 * tasks/nucleos separados sem contaminacao cruzada.
 */
#include <string.h>
#include <math.h>
#include "cv02_synth.h"
#include "cv02_table.h"
#include "cv02_table.h" /* mesma tabela do RX Zephyr - const uint8_t cv02_packed_bits[] */
#include "timecode_formats.h" /* multi-formato: carrier puro sem position tracking */

#define DVS_PI       3.14159265358979323846f
#define SAMPLE_RATE  44444
/* CORRECAO 26/jul: ganho subido de 50% para 60% do fundo de escala
   (meio-termo entre nosso original e o OUTPUT_GAIN=0.70/70% do Felipe),
   mantendo a MESMA PROPORCAO AMP_HIGH/AMP_LOW (~1.41) que ja foi
   calibrada empiricamente antes comparando com foto real do osciloscopio
   do vinil CV02 - preserva a caracteristica visual dos "aneis nitidos"
   ja validada, so aumenta o volume geral. Sem risco de clipping: pico
   maximo (19660) continua bem abaixo do limite int16 (32767). */
#define AMP_HIGH     19660
#define AMP_LOW      13944
#define LFSR_SEED    0x59017
#define LFSR_TAPS    0x361e4

/* IMPORTANTE: 1020.4f (usado no RX Zephyr/nRF52840) NAO deve ser copiado
   aqui as cegas. Aquele valor foi calibrado empiricamente pra compensar
   um desvio de ~2% especifico do CLOCK DAQUELE CHIP (nRF52840) - nao e
   uma propriedade do algoritmo, e do hardware. O ESP32-S3 tem gerador de
   clock I2S completamente diferente, sem motivo nenhum pra ter o mesmo
   desvio. Confirmado por simulacao de host: com 1020.4f, a formula pura
   (sem nenhum desvio de hardware) da ~1019.5Hz teorico, nao 1000Hz - ou
   seja, 1020.4f SO da 1000Hz no nRF52840 especificamente, por coincidir
   com o desvio daquele chip.
   Comecar do valor TEORICO (1000.0f) e recalibrar empiricamente no
   osciloscopio DESTE hardware (ESP32-S3), do zero, independente do que
   foi calibrado no nRF52840. */
#define CV02_RESOLUTION_HZ 1000.0f  /* PENDENTE: recalibrar no osciloscopio deste ESP32-S3 */

#define CV02_LENGTH        1048575UL /* 2^20 - 1: periodo natural do LFSR de 20 bits */
#define CV02_PACKED_BYTES  ((CV02_LENGTH + 7) / 8)
#define CV02_SPAN          ((int64_t)CV02_LENGTH << 16)

#define LINK_TIMEOUT_MS       3000
#define LINK_DECAY_PER_BLOCK  0.98f

#define AMP_CONVERGE_FACTOR 0.20f

/* DECISAO FINAL 26/jul: alpha=1.0 mantem a suavizacao DESATIVADA de
   proposito (vel_smoothed = vel_effective, bypass exato, sem filtro).
   Testes controlados (ganho e deadzone isolados, depois a suavizacao
   isolada) confirmaram que era ELA a causa real de a musica "escapar"
   em comparacao direta contra o Phase DJ - nao o deadzone, nem o ganho.
   Isto NAO e um teste temporario esquecido em 1.0 - e a configuracao de
   producao escolhida. Se um dia quiser reavaliar com suavizacao parcial,
   um alpha mais leve (0.4-0.5) seria o ponto de partida, testado de novo
   contra o Phase antes de adotar. */
#define VEL_SMOOTHING_ALPHA 1.0f

#define SIN_LUT_SIZE 1024
static float sin_lut[SIN_LUT_SIZE];
static bool  sin_lut_ready = false;

_Static_assert(CV02_TABLE_LENGTH >= CV02_LENGTH, "cv02_table.h desatualizado: LENGTH");
_Static_assert(CV02_TABLE_SEED   == LFSR_SEED,   "cv02_table.h desatualizado: SEED");
_Static_assert(CV02_TABLE_TAPS   == LFSR_TAPS,   "cv02_table.h desatualizado: TAPS");
_Static_assert(CV02_TABLE_BYTES  >= CV02_PACKED_BYTES, "cv02_table.h desatualizado: BYTES");

static inline uint8_t get_packed_bit(uint32_t index) {
    uint32_t byte_index = index >> 3;
    uint8_t  bit_mask   = (uint8_t)(1U << (index & 7));
    return (cv02_packed_bits[byte_index] & bit_mask) != 0;
}

/* Mantido para o self-check (nao e codigo morto) - identico ao gen_cv02_table.py */
static uint32_t lfsr_next(uint32_t lfsr) {
    /* xwax: shift RIGHT, bit entra no MSB. */
    uint32_t val = lfsr & (LFSR_TAPS | 1U);
    uint32_t bit = 0;
    while (val) {
        bit ^= val & 1;
        val >>= 1;
    }
    return (lfsr >> 1) | (bit << 19);
}

/* Chamar uma vez no boot, antes do primeiro cv02_fill(). Recomputa os
   primeiros 256 bits ao vivo e compara com a tabela em flash - header
   desatualizado vira erro explicito, nunca timecode silenciosamente
   errado. Retorna false se a tabela nao bater. */
bool cv02_table_selfcheck(void) {
    uint32_t code = LFSR_SEED;
    for (uint32_t i = 0; i < 256; i++) {
        if (get_packed_bit(i) != (uint8_t)(code & 0x1U)) {
            return false;
        }
        code = lfsr_next(code);
    }
    return true;
}

static void build_sin_lut_once(void) {
    if (sin_lut_ready) return;
    for (int i = 0; i < SIN_LUT_SIZE; i++) {
        sin_lut[i] = sinf(((float)i + 0.5f) * (2.0f * DVS_PI / SIN_LUT_SIZE));
    }
    sin_lut_ready = true;
}

void cv02_state_init(cv02_state_t *st) {
    build_sin_lut_once(); /* idempotente - seguro chamar de cada deck */
    memset(st, 0, sizeof(*st));
}

void cv02_fill(cv02_state_t *st, uint32_t *buf, int n, uint32_t now_ms) {
    if ((now_ms - st->last_pkt_ms) <= LINK_TIMEOUT_MS) {
        st->vel_effective = st->current_velocity;
    } else {
        st->vel_effective *= LINK_DECAY_PER_BLOCK;
        if (fabsf(st->vel_effective) < 0.4f) {
            st->vel_effective = 0.0f;
        }
    }

    /* CORRECAO 26/jul: suavizacao exponencial da velocidade (mesma tecnica
       do RPM_SMOOTHING do Felipe), aplicada POR CIMA do resultado do
       timeout de link - suaviza tanto ruido normal de leitura quanto a
       transicao do proprio decaimento de link. */
    st->vel_smoothed += (st->vel_effective - st->vel_smoothed) * VEL_SMOOTHING_ALPHA;
    float vel = st->vel_smoothed;
    /* TESTE 26/jul: revertido temporariamente para 0.4 (valor original),
       mantendo o ganho em 60% - objetivo e confirmar isoladamente se foi
       o deadzone (subido antes para 1.0) que causou a musica "escapando
       mais" percebida no teste contra o Phase DJ, antes de decidir se
       aplicamos a mudanca de deadzone tambem no RX principal (nRF52840). */
    bool active = (fabsf(vel) > 0.4f);
    float amp_scale = active ? 1.0f : 0.0f;

    float ratio = vel / 200.0f;
    int32_t phase_step = active
        ? (int32_t)(ratio * (TC_FORMATS[st->tc_format].res_hz * 65536.0f / SAMPLE_RATE))
        : 0;

    for (int i = 0; i < n; i++) {
        /* NoiseMap removido: carrier puro em quadratura, sem position tracking.
         * Sem LFSR bits -> sem posicao absoluta -> sem fim de disco -> sem INT.
         * Serato rastreia apenas velocidade/direcao pela fase da portadora. */
        uint32_t lut_idx = ((uint32_t)st->phase_fixed >> 6) & 0x3FFu;

        float sine_f   = sin_lut[lut_idx];
        float cosine_f = sin_lut[(lut_idx + (TC_FORMATS[st->tc_format].phase_270 ? 768u : 256u)) & 0x3FFu];

        float amp_target = (float)AMP_HIGH * amp_scale;
        st->amp_current += (amp_target - st->amp_current) * AMP_CONVERGE_FACTOR;

        int16_t sine_v   = (int16_t)( sine_f * st->amp_current);
        int16_t cosine_v = (int16_t)(-cosine_f * st->amp_current);

        int16_t lv, rv;
        if (TC_FORMATS[st->tc_format].swap_lr) {
            lv = sine_v;      /* Traktor: sine no L */
            rv = cosine_v;    /* cosine no R */
        } else {
            lv = cosine_v;    /* Serato/MixVibes/Pioneer: cosine no L */
            rv = sine_v;      /* sine no R */
        }
        buf[i] = ((uint32_t)(uint16_t)lv << 16) | (uint16_t)rv;

        st->phase_fixed += (int64_t)phase_step;
        /* Carrier LUT faz wrap natural com & 0x3FFu - nao precisa de CV02_SPAN */
    }
}
