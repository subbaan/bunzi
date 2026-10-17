#include "level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int level_load(Level *lv, const char *path, char *err, size_t errn)
{
    memset(lv, 0, sizeof *lv);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errn, "cannot open %s", path); return -1; }

    char **rows = NULL;
    int nrows = 0, cap = 0, w = 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == ';') continue;
        size_t n = strcspn(line, "\r\n");
        line[n] = 0;
        if (nrows == cap) {
            cap = cap ? cap * 2 : 32;
            char **nr = realloc(rows, (size_t)cap * sizeof *nr);
            if (!nr) break;
            rows = nr;
        }
        rows[nrows++] = strdup(line);
        if ((int)n > w) w = (int)n;
    }
    fclose(f);
    if (!nrows || !w) {
        snprintf(err, errn, "%s: empty level", path);
        free(rows);
        return -1;
    }

    lv->w = w;
    lv->h = nrows;
    lv->tile = calloc((size_t)w * nrows, 1);
    lv->ch = malloc((size_t)w * nrows);
    lv->start_tx = 2;
    lv->start_ty = 0;
    for (int y = 0; y < nrows; y++) {
        size_t len = strlen(rows[y]);
        for (int x = 0; x < w; x++) {
            char c = x < (int)len ? rows[y][x] : ' ';
            lv->ch[y * w + x] = c;
            uint8_t t = T_EMPTY;
            switch (c) {
            case '#': t = T_SOLID; break;
            case '=': case '_': t = T_PLAT; break;
            case '~': t = T_WATER; break;
            case '^': t = T_SPIKE; break;
            case '@': lv->start_tx = x; lv->start_ty = y; break;
            }
            lv->tile[y * w + x] = t;
        }
        free(rows[y]);
    }
    free(rows);
    /* a thing placed under the surface (a fish) is still in water */
    for (int y = 1; y < nrows; y++)
        for (int x = 0; x < w; x++)
            if (lv->tile[y * w + x] == T_EMPTY && lv->tile[(y - 1) * w + x] == T_WATER)
                lv->tile[y * w + x] = T_WATER;
    return 0;
}

void level_free(Level *lv)
{
    free(lv->tile);
    free(lv->ch);
    memset(lv, 0, sizeof *lv);
}
