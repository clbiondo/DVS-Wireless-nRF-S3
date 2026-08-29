/* timecode_formats.h - Multi-formato carrier puro (sem position tracking)
 * 4 formatos: Serato CV02, Traktor MK1, MixVibes V2, Pioneer RB
 * 15/ago/2026 */

#ifndef TIMECODE_FORMATS_H
#define TIMECODE_FORMATS_H

#include <stdint.h>

typedef struct {
    const char *name;
    float       res_hz;     /* frequencia da portadora */
    uint8_t     phase_270;  /* 1 = 270 graus, 0 = 90 graus */
    uint8_t     swap_lr;    /* 1 = inverte L e R */
} timecode_format_t;

static const timecode_format_t TC_FORMATS[] = {
    { "Serato CV02",  1000.0f, 0, 0 },  /* 90°, sem swap */
    { "Traktor MK1",  2000.0f, 1, 1 },  /* 270°, swap L/R */
    { "MixVibes V2",  1300.0f, 1, 0 },  /* 270°, sem swap */
    { "Pioneer RB",   1000.0f, 0, 0 },  /* 90°, sem swap */
};

#define TC_FORMAT_COUNT  4
#define TC_FORMAT_DEFAULT 0  /* Serato CV02 */

#endif
