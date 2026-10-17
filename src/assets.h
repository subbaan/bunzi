/* PNG images, sprite atlas and sprite/layer drawing. */
#ifndef ASSETS_H
#define ASSETS_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

typedef struct {
    int w, h;
    uint32_t *px;       /* 0xAARRGGBB */
} Image;

typedef struct {
    char name[24];
    int x, y, w, h;     /* rect in the atlas image */
    int ax, ay;         /* anchor inside the rect */
    const Image *img;   /* the atlas image it lives in */
} Sprite;

typedef struct {
    Image img;
    Sprite *spr;
    int n;
} Atlas;

enum { SPR_FLIP = 1, SPR_WHITE = 2, SPR_BLEND = 4 };   /* BLEND: soft alpha instead of a 50% cut */
enum { LAYER_WRAP = 1, LAYER_ALPHA = 2, LAYER_EXTEND = 4 };

int  image_load(Image *im, const char *path);
void image_free(Image *im);
int  atlas_load(Atlas *a, const char *png, const char *txt);
void atlas_free(Atlas *a);
const Sprite *atlas_find(const Atlas *a, const char *name);

/* Draw so the sprite's anchor lands on (x, y). shade mixes toward shade_to (0..256). */
void draw_sprite(Canvas *cv, const Sprite *s, int x, int y, int flags,
                 uint32_t shade_to, int shade);

/* Restrict draw_sprite to a screen rectangle; sprite_noclip() lifts it. */
void sprite_clip(int x0, int y0, int x1, int y1);
void sprite_noclip(void);

/* Repeat a sprite left to right from x0 (its anchor ignored) and cut it at x1. */
void draw_hstrip(Canvas *cv, const Sprite *s, int x0, int x1, int top,
                 uint32_t shade_to, int shade);

/* Screen pixel (x, y) samples image (x + ox, y + oy). LAYER_WRAP mirror-tiles
 * horizontally; LAYER_ALPHA skips transparent pixels; LAYER_EXTEND repeats the
 * bottom row below the image. */
void draw_layer(Canvas *cv, const Image *im, int ox, int oy, int flags,
                uint32_t shade_to, int shade);

#endif
