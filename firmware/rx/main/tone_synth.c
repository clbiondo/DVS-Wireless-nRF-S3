/* tone_synth.c - Sintese de tom direcional puro (sem NoiseMap/modo
 * absoluto). Ver justificativa completa em tone_synth.h.
 *
 * Reaproveita a mesma matematica de fase da versao completa (cv02_synth):
 * acumulador int64_t com wrap condicional, LUT de seno de 1024 entradas.
 * A UNICA diferenca real e a ausencia da modulacao de amplitude por bit
 * do LFSR/tabela CV02 - aqui a amplitude so' converge suavemente entre
 * "ativo" e "inativo" (mesmo AMP_CONVERGE_FACTOR de antes), sem alternar
 * entre dois niveis por bit.
 *
 * Sem tabela CV02, sem cv02_table.h, sem LFSR - CV02_LENGTH nao existe
 * mais aqui, so a frequencia da portadora (mesma formula/constante
 * recalibravel que ja tinhamos).
 */
#include <string.h>
#include <math.h>
#include "tone_synth.h"

#define DVS_PI       3.14159265358979323846f
#define SAMPLE_RATE  44444

/* Mesma logica de recalibracao ja estabelecida: comecar do valor
   TEORICO, recalibrar no osciloscopio deste hardware especifico -
   nao copiar as cegas o 1020.4f do nRF52840 nem qualquer outro valor
   de outro chip (ver nota identica em cv02_synth.c). */
#define TONE_RESOLUTION_HZ 1000.0f  /* PENDENTE: recalibrar no osciloscopio */

#define LINK_TIMEOUT_MS       3000
#define LINK_DECAY_PER_BLOCK  0.98f

#define AMP_CONVERGE_FACTOR 0.20f
#define AMP_LEVEL 19660  /* mesmo pico de 60% do fundo de escala ja calibrado */

#define SIN_LUT_SIZE 1024
static float sin_lut[SIN_LUT_SIZE];
static bool  sin_lut_ready = false;

static void build_sin_lut_once(void) {
    if (sin_lut_ready) return;
    for (int i = 0; i < SIN_LUT_SIZE; i++) {
        sin_lut[i] = sinf(((float)i + 0.5f) * (2.0f * DVS_PI / SIN_LUT_SIZE));
    }
    sin_lut_ready = true;
}

void tone_state_init(tone_state_t *st) {
    build_sin_lut_once();
    memset(st, 0, sizeof(*st));
}

void tone_fill(tone_state_t *st, uint32_t *buf, int n, uint32_t now_ms) {
    if ((now_ms - st->last_pkt_ms) <= LINK_TIMEOUT_MS) {
        st->vel_effective = st->current_velocity;
    } else {
        st->vel_effective *= LINK_DECAY_PER_BLOCK;
        if (fabsf(st->vel_effective) < 0.4f) {
            st->vel_effective = 0.0f;
        }
    }

    float vel = st->vel_effective;
    bool active = (fabsf(vel) > 0.4f);
    float amp_scale = active ? 1.0f : 0.0f;

    float ratio = vel / 200.0f;
    int32_t phase_step = active
        ? (int32_t)(ratio * (TONE_RESOLUTION_HZ * 65536.0f / SAMPLE_RATE))
        : 0;

    /* CORRECAO: sem NoiseMap - amplitude alvo e' UM UNICO nivel (nao
       alterna entre AMP_HIGH/AMP_LOW por bit de LFSR), so' liga/desliga
       suavemente via a mesma convergencia exponencial de antes. */
    float amp_target_level = (float)AMP_LEVEL * amp_scale;

    for (int i = 0; i < n; i++) {
        uint32_t lut_idx = ((uint32_t)st->phase_fixed >> 6) & 0x3FFu;

        float sine_f   = sin_lut[lut_idx];
        float cosine_f = sin_lut[(lut_idx + 256u) & 0x3FFu];

        st->amp_current += (amp_target_level - st->amp_current) * AMP_CONVERGE_FACTOR;

        int16_t sine_v   = (int16_t)( sine_f * st->amp_current);
        int16_t cosine_v = (int16_t)(-cosine_f * st->amp_current);

        int16_t lv = cosine_v;
        int16_t rv = sine_v;
        buf[i] = ((uint32_t)(uint16_t)lv << 16) | (uint16_t)rv;

        st->phase_fixed += (int64_t)phase_step;
        /* Sem CV02_LENGTH/tabela, nao precisamos de wrap explicito: a
           extracao de lut_idx (bits 6..15) e' valida independente de
           quanto phase_fixed cresca em int64_t - so' os bits baixos
           importam pra fase do seno/cosseno, e o int64_t tem folga
           gigante antes de qualquer overflow real (anos de uso continuo
           antes de chegar perto do limite). Diferente da versao com
           NoiseMap, aqui nao existe "posicao numa tabela grande" pra
           preservar - so' um ciclo repetindo, entao nao ha necessidade
           de modulo nem de span explicito. */
    }
}
