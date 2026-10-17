#include "assets.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int image_load(Image *im, const char *path)
{
    memset(im, 0, sizeof *im);
    png_image p;
    memset(&p, 0, sizeof p);
    p.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&p, path)) return -1;
    p.format = PNG_FORMAT_BGRA;     /* little-endian uint32 0xAARRGGBB */
    im->px = malloc(PNG_IMAGE_SIZE(p));
    if (!im->px) { png_image_free(&p); return -1; }
    if (!png_image_finish_read(&p, NULL, im->px, 0, NULL)) {
        free(im->px);
        im->px = NULL;
        return -1;
    }
    im->w = (int)p.width;
    im->h = (int)p.height;
    return 0;
}

void image_free(Image *im)
{
    free(im->px);
    memset(im, 0, sizeof *im);
}

int atlas_load(Atlas *a, const char *png, const char *txt)
{
    memset(a, 0, sizeof *a);
    if (image_load(&a->img, png)) return -1;
    FILE *f = fopen(txt, "r");
    if (!f) { image_free(&a->img); return -1; }
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        Sprite s;
        if (sscanf(line, "%23s %d %d %d %d %d %d", s.name, &s.x, &s.y, &s.w, &s.h, &s.ax, &s.ay) != 7)
            continue;
        if (s.x < 0 || s.y < 0 || s.x + s.w > a->img.w || s.y + s.h > a->img.h) continue;
        if (a->n == cap) {
            cap = cap ? cap * 2 : 128;
            Sprite *ns = realloc(a->spr, (size_t)cap * sizeof *ns);
            if (!ns) { fclose(f); atlas_free(a); return -1; }
            a->spr = ns;
        }
        a->spr[a->n++] = s;
    }
    fclose(f);
    for (int i = 0; i < a->n; i++) a->spr[i].img = &a->img;
    return 0;
}

void atlas_free(Atlas *a)
{
    image_free(&a->img);
    free(a->spr);
    memset(a, 0, sizeof *a);
}

const Sprite *atlas_find(const Atlas *a, const char *name)
{
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->spr[i].name, name)) return &a->spr[i];
    return NULL;
}

static int clip_x0, clip_y0, clip_x1 = 1 << 30, clip_y1 = 1 << 30;

void sprite_clip(int x0, int y0, int x1, int y1)
{
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
}

void sprite_noclip(void)
{
    clip_x0 = clip_y0 = 0;
    clip_x1 = clip_y1 = 1 << 30;
}

void draw_sprite(Canvas *cv, const Sprite *s, int x, int y, int flags,
                 uint32_t shade_to, int shade)
{
    if (!s || !s->img) return;
    const Image *im = s->img;
    int top = y - s->ay;
    for (int j = 0; j < s->h; j++) {
        int sy = top + j;
        if ((unsigned)sy >= (unsigned)cv->h || sy < clip_y0 || sy >= clip_y1) continue;
        const uint32_t *src = im->px + (size_t)(s->y + j) * im->w + s->x;
        uint32_t *dst = cv->px + (size_t)sy * cv->w;
        for (int i = 0; i < s->w; i++) {
            uint32_t c = src[i];
            int al = (int)(c >> 24);
            if (al < ((flags & SPR_BLEND) ? 4 : 128)) continue;
            int sx = (flags & SPR_FLIP) ? x + (s->ax - i) : x - s->ax + i;
            if ((unsigned)sx >= (unsigned)cv->w || sx < clip_x0 || sx >= clip_x1) continue;
            c &= 0xffffff;
            if (flags & SPR_WHITE) c = 0xffffff;
            else if (shade) c = gfx_mix(c, shade_to, shade);
            dst[sx] = (flags & SPR_BLEND) && al < 252 ? gfx_mix(dst[sx], c, al + (al >> 7)) : c;
        }
    }
}

void draw_hstrip(Canvas *cv, const Sprite *s, int x0, int x1, int top,
                 uint32_t shade_to, int shade)
{
    if (!s || !s->img || s->w <= 0) return;
    const Image *im = s->img;
    for (int j = 0; j < s->h; j++) {
        int sy = top + j;
        if ((unsigned)sy >= (unsigned)cv->h) continue;
        const uint32_t *src = im->px + (size_t)(s->y + j) * im->w + s->x;
        uint32_t *dst = cv->px + (size_t)sy * cv->w;
        for (int sx = x0 < 0 ? 0 : x0; sx < x1 && sx < cv->w; sx++) {
            uint32_t c = src[(sx - x0) % s->w];
            if ((c >> 24) < 128) continue;
            c &= 0xffffff;
            dst[sx] = shade ? gfx_mix(c, shade_to, shade) : c;
        }
    }
}

void draw_layer(Canvas *cv, const Image *im, int ox, int oy, int flags,
                uint32_t shade_to, int shade)
{
    int w2 = im->w * 2;
    for (int y = 0; y < cv->h; y++) {
        int iy = y + oy;
        if (iy >= im->h && (flags & LAYER_EXTEND)) iy = im->h - 1;
        if (iy < 0 || iy >= im->h) continue;
        const uint32_t *src = im->px + (size_t)iy * im->w;
        uint32_t *dst = cv->px + (size_t)y * cv->w;
        for (int x = 0; x < cv->w; x++) {
            int ix = x + ox;
            if (flags & LAYER_WRAP) {
                ix %= w2;
                if (ix < 0) ix += w2;
                if (ix >= im->w) ix = w2 - 1 - ix;
            } else if (ix < 0 || ix >= im->w) {
                continue;
            }
            uint32_t c = src[ix];
            if ((flags & LAYER_ALPHA) && (c >> 24) < 128) continue;
            c &= 0xffffff;
            if (shade) c = gfx_mix(c, shade_to, shade);
            dst[x] = c;
        }
    }
}
