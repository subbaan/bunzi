#include "gfx.h"

#include <stdarg.h>
#include <stdio.h>

/* 3x5 glyphs, rows top to bottom, '1' = ink. Lowercase maps to uppercase. */
static const char *const GLYPH[96] = {
    [' ' - 32] = "000000000000000",
    ['!' - 32] = "010010010000010",
    ['%' - 32] = "101001010100101",
    ['(' - 32] = "001010010010001",
    [')' - 32] = "100010010010100",
    ['+' - 32] = "000010111010000",
    [',' - 32] = "000000000010100",
    ['-' - 32] = "000000111000000",
    ['.' - 32] = "000000000000010",
    ['/' - 32] = "001001010100100",
    ['0' - 32] = "111101101101111",
    ['1' - 32] = "010110010010111",
    ['2' - 32] = "111001111100111",
    ['3' - 32] = "111001111001111",
    ['4' - 32] = "101101111001001",
    ['5' - 32] = "111100111001111",
    ['6' - 32] = "111100111101111",
    ['7' - 32] = "111001001001001",
    ['8' - 32] = "111101111101111",
    ['9' - 32] = "111101111001111",
    [':' - 32] = "000010000010000",
    ['<' - 32] = "001010100010001",
    ['=' - 32] = "000111000111000",
    ['>' - 32] = "100010001010100",
    ['?' - 32] = "111001010000010",
    ['A' - 32] = "010101111101101",
    ['B' - 32] = "110101110101110",
    ['C' - 32] = "011100100100011",
    ['D' - 32] = "110101101101110",
    ['E' - 32] = "111100110100111",
    ['F' - 32] = "111100110100100",
    ['G' - 32] = "011100101101011",
    ['H' - 32] = "101101111101101",
    ['I' - 32] = "111010010010111",
    ['J' - 32] = "001001001101010",
    ['K' - 32] = "101101110101101",
    ['L' - 32] = "100100100100111",
    ['M' - 32] = "101111111101101",
    ['N' - 32] = "110101101101101",
    ['O' - 32] = "010101101101010",
    ['P' - 32] = "110101110100100",
    ['Q' - 32] = "010101101110011",
    ['R' - 32] = "110101110101101",
    ['S' - 32] = "011100010001110",
    ['T' - 32] = "111010010010010",
    ['U' - 32] = "101101101101111",
    ['V' - 32] = "101101101101010",
    ['W' - 32] = "101101111111101",
    ['X' - 32] = "101101010101101",
    ['Y' - 32] = "101101010010010",
    ['Z' - 32] = "111001010100111",
    ['[' - 32] = "011010010010011",
    [']' - 32] = "110010010010110",
    ['^' - 32] = "010101000000000",
    ['_' - 32] = "000000000000111",
};

const char *gfx_glyph(unsigned ch)
{
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    return ch >= 32 && ch < 128 ? GLYPH[ch - 32] : NULL;
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

uint32_t gfx_shade(uint32_t c, int v)
{
    int r = clamp255((int)(c >> 16 & 255) + v);
    int g = clamp255((int)(c >> 8 & 255) + v);
    int b = clamp255((int)(c & 255) + v);
    return (uint32_t)(r << 16 | g << 8 | b);
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int t)
{
    int r = (int)(a >> 16 & 255), g = (int)(a >> 8 & 255), bl = (int)(a & 255);
    r  += (((int)(b >> 16 & 255) - r) * t) >> 8;
    g  += (((int)(b >> 8 & 255) - g) * t) >> 8;
    bl += (((int)(b & 255) - bl) * t) >> 8;
    return (uint32_t)(r << 16 | g << 8 | bl);
}

void gfx_fill(Canvas *cv, int x, int y, int w, int h, uint32_t c)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > cv->w) x1 = cv->w;
    if (y1 > cv->h) y1 = cv->h;
    for (int j = y; j < y1; j++)
        for (int i = x; i < x1; i++) cv->px[j * cv->w + i] = c;
}

void gfx_tint(Canvas *cv, int x, int y, int w, int h, uint32_t c, int t)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > cv->w) x1 = cv->w;
    if (y1 > cv->h) y1 = cv->h;
    for (int j = y; j < y1; j++)
        for (int i = x; i < x1; i++)
            cv->px[j * cv->w + i] = gfx_mix(cv->px[j * cv->w + i], c, t);
}

int gfx_text(Canvas *cv, int x, int y, const char *s, uint32_t c)
{
    return gfx_text_scaled(cv, x, y, 1, s, c);
}

int gfx_text_width(const char *s, int scale)
{
    int n = 0;
    for (; *s && *s != '\n'; s++) n++;
    return n ? (n * 4 - 1) * scale : 0;
}

void gfx_glow(Canvas *cv, int x, int y, int r, uint32_t c, int strength)
{
    int cr = (int)(c >> 16 & 255), cg = (int)(c >> 8 & 255), cb = (int)(c & 255);
    for (int dy = -r; dy <= r; dy++) {
        int py = y + dy;
        if ((unsigned)py >= (unsigned)cv->h) continue;
        for (int dx = -r; dx <= r; dx++) {
            int px = x + dx;
            if ((unsigned)px >= (unsigned)cv->w) continue;
            int d2 = dx * dx + dy * dy;
            if (d2 >= r * r) continue;
            int f = 256 - d2 * 256 / (r * r);
            f = f * f / 256 * strength / 256;
            uint32_t *p = &cv->px[py * cv->w + px];
            int R = (int)(*p >> 16 & 255) + cr * f / 256;
            int G = (int)(*p >> 8 & 255) + cg * f / 256;
            int B = (int)(*p & 255) + cb * f / 256;
            *p = (uint32_t)(clamp255(R) << 16 | clamp255(G) << 8 | clamp255(B));
        }
    }
}

int gfx_text_scaled(Canvas *cv, int x, int y, int scale, const char *s, uint32_t c)
{
    int cx = x;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch == '\n') { y += 6 * scale; cx = x; continue; }
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        const char *g = ch >= 32 && ch < 128 ? GLYPH[ch - 32] : NULL;
        if (g)
            for (int r = 0; r < 5; r++)
                for (int k = 0; k < 3; k++)
                    if (g[r * 3 + k] == '1') gfx_fill(cv, cx + k * scale, y + r * scale, scale, scale, c);
        cx += 4 * scale;
    }
    return cx;
}

int gfx_textf(Canvas *cv, int x, int y, uint32_t c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gfx_text(cv, x + 1, y + 1, buf, 0x000000);
    return gfx_text(cv, x, y, buf, c);
}
