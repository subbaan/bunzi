/* Text-format tile levels: one character per 16px tile, ';' starts a comment line.
 * '#' solid, '=' and '_' one-way platforms, '^' spikes, '~' water, '@' start. */
#ifndef LEVEL_H
#define LEVEL_H

#include <stddef.h>
#include <stdint.h>

#define TS 16

enum { T_EMPTY, T_SOLID, T_PLAT, T_SPIKE, T_WATER };

typedef struct {
    int w, h;
    uint8_t *tile;      /* T_* per cell */
    char *ch;           /* original character per cell */
    int start_tx, start_ty;
} Level;

int  level_load(Level *lv, const char *path, char *err, size_t errn);
void level_free(Level *lv);

static inline int level_at(const Level *lv, int tx, int ty)
{
    if (tx < 0 || tx >= lv->w) return T_SOLID;
    if (ty < 0 || ty >= lv->h) return T_EMPTY;
    return lv->tile[ty * lv->w + tx];
}

static inline char level_ch(const Level *lv, int tx, int ty)
{
    if (tx < 0 || tx >= lv->w || ty < 0 || ty >= lv->h) return ' ';
    return lv->ch[ty * lv->w + tx];
}

#endif
