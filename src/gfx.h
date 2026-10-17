/* Software framebuffer drawing. Pixels are 0xRRGGBB. */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

typedef struct {
    int w, h;
    uint32_t *px;
} Canvas;

static inline void gfx_put(Canvas *cv, int x, int y, uint32_t c)
{
    if ((unsigned)x < (unsigned)cv->w && (unsigned)y < (unsigned)cv->h)
        cv->px[y * cv->w + x] = c;
}

uint32_t gfx_shade(uint32_t c, int v);              /* add v to each channel */
uint32_t gfx_mix(uint32_t a, uint32_t b, int t);    /* t: 0 = a .. 256 = b */
void gfx_fill(Canvas *cv, int x, int y, int w, int h, uint32_t c);
void gfx_tint(Canvas *cv, int x, int y, int w, int h, uint32_t c, int t);
int  gfx_text(Canvas *cv, int x, int y, const char *s, uint32_t c);   /* 3x5 font */
int  gfx_text_scaled(Canvas *cv, int x, int y, int scale, const char *s, uint32_t c);
int  gfx_text_width(const char *s, int scale);
const char *gfx_glyph(unsigned ch);     /* 15 chars of '0'/'1' (3x5), NULL if none */
void gfx_glow(Canvas *cv, int x, int y, int r, uint32_t c, int strength);  /* additive */
int  gfx_textf(Canvas *cv, int x, int y, uint32_t c, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

#endif
