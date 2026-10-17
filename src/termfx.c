#include "termfx.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* intro: a long look at the terminal with the cursor glowing, its spark, then
 * a slow wave of characters that lift off as letters before turning to light */
#define HOLD        1.00f
#define WAVE_START  1.40f
#define WAVE_LEN    3.00f
#define JITTER      0.35f
#define COVER_FADE  0.45f
#define FRAG_LIFE   2.00f
#define GLYPH_T     0.75f       /* how long a character floats as itself */
/* outro: characters fly home, the cursor last */
#define OUT_START   0.40f
#define OUT_WAVE    1.80f
#define LAND_FADE   0.25f

struct Frag {
    float x, y;             /* cell centre, pixels */
    float vx, vy, sway, phase, when, life;
    uint32_t col;
    const char *glyph;
    bool firefly;
};

/* ------------------------------------------------------------- capture */

static bool safe_arg(const char *s, const char *extra)
{
    if (!s || !*s || strlen(s) > 200) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr(extra, *s)))
            return false;
    return true;
}

static char *run(const char *cmd, size_t *len)
{
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 65536, n = 0;
    char *b = malloc(cap);
    size_t r;
    while (b && (r = fread(b + n, 1, cap - n - 1, p)) > 0) {
        n += r;
        if (cap - n < 4096) {
            char *nb = realloc(b, cap * 2);
            if (!nb) { free(b); b = NULL; break; }
            b = nb;
            cap *= 2;
        }
    }
    int st = pclose(p);
    if (!b || st != 0) { free(b); return NULL; }
    b[n] = 0;
    *len = n;
    return b;
}

static uint32_t xterm256(int n)
{
    static const uint32_t base[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
    };
    if (n < 16) return base[n];
    if (n < 232) {
        n -= 16;
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return (uint32_t)(lv[n / 36] << 16 | lv[n / 6 % 6] << 8 | lv[n % 6]);
    }
    int v = 8 + (n - 232) * 10;
    return (uint32_t)(v << 16 | v << 8 | v);
}

typedef struct { int v[8]; int n; } Group;    /* one ';' parameter with ':' sub-parameters */

static int parse_groups(const char *p, const char *end, Group *g, int maxg)
{
    int ng = 0;
    g[0].n = 0;
    int cur = -1;
    for (; p <= end; p++) {
        char c = p < end ? *p : ';';
        if (c >= '0' && c <= '9') { cur = (cur < 0 ? 0 : cur) * 10 + (c - '0'); continue; }
        if (c == ':' || c == ';') {
            if (g[ng].n < 8) g[ng].v[g[ng].n++] = cur;
            cur = -1;
            if (c == ';') {
                if (++ng >= maxg) return ng;
                g[ng].n = 0;
            }
        }
    }
    return ng;
}

static void sgr(const Group *g, int ng, const uint32_t *pal, int *fg, int *bg, bool *rev)
{
    for (int i = 0; i < ng; i++) {
        int c = g[i].v[0] < 0 ? 0 : g[i].v[0];
        if (c == 38 || c == 48) {
            int *dst = c == 38 ? fg : bg, val = -1;
            if (g[i].n > 1) {                       /* colon form: 38:5:n or 38:2:[cs:]r:g:b */
                if (g[i].v[1] == 5 && g[i].n > 2) val = (int)pal[g[i].v[2] & 255];
                else if (g[i].v[1] == 2 && g[i].n >= 5) {
                    int k = g[i].n >= 6 ? 3 : 2;
                    val = (g[i].v[k] & 255) << 16 | (g[i].v[k + 1] & 255) << 8 | (g[i].v[k + 2] & 255);
                }
            } else if (i + 1 < ng) {                /* semicolon form */
                int m = g[i + 1].v[0];
                if (m == 5 && i + 2 < ng) { val = (int)pal[g[i + 2].v[0] & 255]; i += 2; }
                else if (m == 2 && i + 4 < ng) {
                    val = (g[i + 2].v[0] & 255) << 16 | (g[i + 3].v[0] & 255) << 8 | (g[i + 4].v[0] & 255);
                    i += 4;
                }
            }
            if (val >= 0) *dst = val;
        } else if (c == 0) { *fg = *bg = -1; *rev = false; }
        else if (c == 7) *rev = true;
        else if (c == 27) *rev = false;
        else if (c >= 30 && c <= 37) *fg = (int)pal[c - 30];
        else if (c >= 90 && c <= 97) *fg = (int)pal[c - 90 + 8];
        else if (c == 39) *fg = -1;
        else if (c >= 40 && c <= 47) *bg = (int)pal[c - 40];
        else if (c >= 100 && c <= 107) *bg = (int)pal[c - 100 + 8];
        else if (c == 49) *bg = -1;
    }
}

static void parse_text(TermFx *fx, const char *s, size_t n, const uint32_t *pal)
{
    int fg = -1, bg = -1, row = 0, col = 0;
    bool rev = false;
    mbstate_t mb;
    memset(&mb, 0, sizeof mb);
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2;
            bool priv = j < n && (s[j] == '?' || s[j] == '>');
            if (priv) j++;
            size_t p0 = j;
            while (j < n && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e)) j++;
            if (j >= n) break;
            char fin = s[j];
            Group g[32];
            if (!priv && fin == 'm') {
                int ng = parse_groups(s + p0, s + j, g, 32);
                sgr(g, ng, pal, &fg, &bg, &rev);
            } else if (!priv && fin == 'H') {
                int ng = parse_groups(s + p0, s + j, g, 32);
                fx->cur_r = (ng > 0 && g[0].v[0] > 0 ? g[0].v[0] : 1) - 1;
                fx->cur_c = (ng > 1 && g[1].v[0] > 0 ? g[1].v[0] : 1) - 1;
            }
            i = j + 1;
            continue;
        }
        if (c == 0x1b) { i += 2; continue; }
        if (c == '\n') { row++; col = 0; i++; continue; }
        if (c < 0x20) { i++; continue; }
        wchar_t wc;
        size_t k = mbrtowc(&wc, s + i, n - i, &mb);
        if (k == (size_t)-1 || k == (size_t)-2 || k == 0) { memset(&mb, 0, sizeof mb); i++; continue; }
        i += k;
        int w = wcwidth(wc);
        if (w <= 0) continue;
        if (row < fx->rows && col < fx->cols) {
            uint32_t f = fg < 0 ? fx->fg : (uint32_t)fg, b = bg < 0 ? fx->bg : (uint32_t)bg;
            bool hb = bg >= 0;
            if (rev) { uint32_t t = f; f = b; b = t; hb = true; }
            for (int q = 0; q < w && col + q < fx->cols; q++)
                fx->cell[row * fx->cols + col + q] = (TCell){ q ? 0 : (uint32_t)wc, f, b, hb };
        }
        col += w;
    }
}

int termfx_capture(TermFx *fx, const TermSize *ts)
{
    memset(fx, 0, sizeof *fx);
    if (ts->cols <= 0 || ts->rows <= 0 || ts->cellw <= 0 || ts->cellh <= 0) {
        snprintf(fx->status, sizeof fx->status, "no window size");
        return -1;
    }
    fx->cols = ts->cols;
    fx->rows = ts->rows;
    fx->cellw = ts->cellw;
    fx->cellh = ts->cellh;
    fx->W = fx->cols * fx->cellw;
    fx->H = fx->rows * fx->cellh;
    fx->fg = 0xd0d0d0;
    fx->bg = 0x101010;
    fx->cur_r = fx->rows - 1;
    fx->cell = calloc((size_t)fx->cols * fx->rows, sizeof *fx->cell);
    fx->when = calloc((size_t)fx->cols * fx->rows, sizeof *fx->when);
    fx->buf = malloc((size_t)fx->W * fx->H * 4);
    if (!fx->cell || !fx->when || !fx->buf) { termfx_free(fx); return -1; }

    const char *to = getenv("KITTY_LISTEN_ON"), *wid = getenv("KITTY_WINDOW_ID");
    if (!safe_arg(to, ":/._-@") || !safe_arg(wid, "")) {
        snprintf(fx->status, sizeof fx->status, "no kitty remote control socket: sweep only");
        return 0;
    }
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = xterm256(i);
    char cmd[640];
    size_t n = 0;
    snprintf(cmd, sizeof cmd, "timeout 2 kitten @ --to '%s' get-colors --match 'id:%s' 2>/dev/null", to, wid);
    char *cols = run(cmd, &n);
    if (cols) {
        for (char *l = strtok(cols, "\n"); l; l = strtok(NULL, "\n")) {
            char name[64];
            unsigned v;
            if (sscanf(l, "%63s #%x", name, &v) != 2) continue;
            int k;
            if (!strcmp(name, "foreground")) fx->fg = v;
            else if (!strcmp(name, "background")) fx->bg = v;
            else if (sscanf(name, "color%d", &k) == 1 && k >= 0 && k < 256) pal[k] = v;
        }
        free(cols);
    }
    snprintf(cmd, sizeof cmd, "timeout 2 kitten @ --to '%s' get-text --match 'id:%s' --ansi --add-cursor "
             "--extent screen 2>/dev/null", to, wid);
    char *text = run(cmd, &n);
    if (!text) {
        snprintf(fx->status, sizeof fx->status, "kitty get-text refused: sweep only");
        return 0;
    }
    for (size_t i = 0; i < (size_t)fx->cols * fx->rows; i++)
        fx->cell[i] = (TCell){ ' ', fx->fg, fx->bg, false };
    setlocale(LC_CTYPE, "");
    parse_text(fx, text, n, pal);
    memset(text, 0, n);         /* terminal contents never outlive the parse */
    free(text);
    fx->have_text = true;
    int glyphs = 0;
    for (int i = 0; i < fx->cols * fx->rows; i++) glyphs += fx->cell[i].ch > ' ';
    snprintf(fx->status, sizeof fx->status, "%d characters from kitty, cursor %d,%d",
             glyphs, fx->cur_r + 1, fx->cur_c + 1);
    return 0;
}

void termfx_free(TermFx *fx)
{
    if (fx->cell) memset(fx->cell, 0, (size_t)fx->cols * fx->rows * sizeof *fx->cell);
    free(fx->cell);
    free(fx->when);
    free(fx->buf);
    free(fx->frag);
    memset(fx, 0, sizeof *fx);
}

/* --------------------------------------------------------- choreography */

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float rnd(uint32_t h) { return (h & 0xffff) / 65535.0f; }

float termfx_intro_len(const TermFx *fx)
{
    (void)fx;
    return WAVE_START + WAVE_LEN + JITTER + FRAG_LIFE;
}

float termfx_outro_len(const TermFx *fx)
{
    (void)fx;
    return OUT_START + OUT_WAVE + JITTER + LAND_FADE + 0.35f;
}

static void add_frag(TermFx *fx, struct Frag f)
{
    if (fx->nfrag == fx->capfrag) {
        int nc = fx->capfrag ? fx->capfrag * 2 : 512;
        struct Frag *nf = realloc(fx->frag, (size_t)nc * sizeof *nf);
        if (!nf) return;
        fx->frag = nf;
        fx->capfrag = nc;
    }
    fx->frag[fx->nfrag++] = f;
}

void termfx_start(TermFx *fx, bool outro, int cw, int chh)
{
    /* the game canvas is centred at an integer scale, exactly like kgfx_present */
    int s = fx->W / cw;
    if (fx->H / chh < s) s = fx->H / chh;
    if (s < 1) s = 1;
    fx->s = s;
    fx->ox = (fx->W - cw * s) / 2;
    fx->oy = (fx->H - chh * s) / 2;
    fx->nfrag = 0;

    /* distance from the cursor sets the order; a soft diagonal sweep without text */
    float cx = (fx->cur_c + 0.5f) * fx->cellw, cy = (fx->cur_r + 0.5f) * fx->cellh;
    float maxd = 1;
    for (int r = 0; r < fx->rows; r++)
        for (int c = 0; c < fx->cols; c++) {
            float dx = (c + 0.5f) * fx->cellw - cx, dy = (r + 0.5f) * fx->cellh - cy;
            float d = fx->have_text ? sqrtf(dx * dx + dy * dy)
                                    : (float)c * fx->cellw + (float)(fx->rows - r) * fx->cellh * 1.5f;
            fx->when[r * fx->cols + c] = d;
            if (d > maxd) maxd = d;
        }
    for (int r = 0; r < fx->rows; r++)
        for (int c = 0; c < fx->cols; c++) {
            float d = fx->when[r * fx->cols + c] / maxd;
            uint32_t h = hash2(c, r);
            float j = rnd(h) * JITTER;
            fx->when[r * fx->cols + c] = outro ? OUT_START + (1 - d) * OUT_WAVE + j
                                               : WAVE_START + d * WAVE_LEN + j;
            const TCell *tc = &fx->cell[r * fx->cols + c];
            bool ink = fx->have_text ? tc->ch > ' ' : rnd(h >> 7) < 0.12f;
            if (!ink) continue;
            struct Frag f;
            f.x = (c + 0.5f) * fx->cellw;
            f.y = (r + 0.5f) * fx->cellh;
            f.vx = (rnd(h >> 3) - 0.5f) * fx->cellw * 3.0f;
            f.vy = -(1.5f + rnd(h >> 5) * 3.0f) * fx->cellh;
            f.sway = (0.5f + rnd(h >> 9)) * fx->cellw;
            f.phase = rnd(h >> 11) * 6.28f;
            f.when = fx->when[r * fx->cols + c];
            f.firefly = rnd(h >> 13) < 0.18f;
            f.life = FRAG_LIFE * (f.firefly ? 1.0f : 0.55f + 0.45f * rnd(h >> 15));
            f.col = fx->have_text ? tc->fg : fx->fg;
            f.glyph = fx->have_text ? gfx_glyph(tc->ch) : NULL;
            add_frag(fx, f);
        }
    /* the cursor is the first spark in, the last one home */
    struct Frag k = { (fx->cur_c + 0.5f) * fx->cellw, (fx->cur_r + 0.5f) * fx->cellh,
                      fx->cellw * 0.6f, -2.2f * fx->cellh, fx->cellw * 1.5f, 0,
                      outro ? termfx_outro_len(fx) - LAND_FADE - 0.2f : HOLD, 1.5f, 0xffd27a, NULL, true };
    add_frag(fx, k);
}

/* ------------------------------------------------------------ rendering */

/* "over" compositing onto non-premultiplied ARGB */
static inline void blend(uint32_t *d, uint32_t c, int a)
{
    if (a <= 0) return;
    if (a > 255) a = 255;
    int da = (int)(*d >> 24);
    if (da == 0 || a == 255) { *d = (uint32_t)a << 24 | (c & 0xffffff); return; }
    int oa = a + da * (255 - a) / 255;
    int r = (((int)(c >> 16 & 255) * a) + ((int)(*d >> 16 & 255) * da * (255 - a) / 255)) / oa;
    int g = (((int)(c >> 8 & 255) * a) + ((int)(*d >> 8 & 255) * da * (255 - a) / 255)) / oa;
    int b = (((int)(c & 255) * a) + ((int)(*d & 255) * da * (255 - a) / 255)) / oa;
    *d = (uint32_t)oa << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static void blob(TermFx *fx, int x, int y, int w, int h, uint32_t c, int a)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < fx->H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < fx->W; i++)
            blend(&fx->buf[(size_t)j * fx->W + i], c, a);
}

static void glow(TermFx *fx, float x, float y, float r, uint32_t c, int a)
{
    int x0 = (int)(x - r), x1 = (int)(x + r), y0 = (int)(y - r), y1 = (int)(y + r);
    for (int j = y0 < 0 ? 0 : y0; j <= y1 && j < fx->H; j++)
        for (int i = x0 < 0 ? 0 : x0; i <= x1 && i < fx->W; i++) {
            float dx = i - x, dy = j - y, d2 = (dx * dx + dy * dy) / (r * r);
            if (d2 >= 1) continue;
            float f = (1 - d2) * (1 - d2);
            blend(&fx->buf[(size_t)j * fx->W + i], c, (int)(a * f));
        }
}

static uint32_t mixc(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int k = (int)(t * 256);
    return gfx_mix(a, b, k);
}

static void draw_frag(TermFx *fx, const struct Frag *f, float tau)
{
    if (tau < 0 || tau > f->life) return;
    int p = fx->s;                                  /* one game pixel */
    if (p > fx->cellw / 3) p = fx->cellw / 3;
    if (p < 1) p = 1;
    float x = f->x + f->vx * tau + f->sway * sinf(f->phase + tau * 5.0f) * tau;
    float y = f->y + f->vy * tau - 0.8f * fx->cellh * tau * tau;
    float fade = tau > f->life * 0.55f ? 1 - (tau - f->life * 0.55f) / (f->life * 0.45f) : 1;
    uint32_t warm = f->firefly ? ((f->phase > 3.14f) ? 0x9af0c8 : 0xffd27a) : 0xffe6b0;
    if (tau < GLYPH_T && f->glyph) {
        /* the character itself, as game pixels, flaring as it lets go and
         * warming as it rises */
        float g = tau / GLYPH_T;
        uint32_t c = g < 0.15f ? mixc(f->col, 0xffffff, g / 0.15f) : mixc(0xffffff, warm, (g - 0.15f) * 1.2f);
        int gx = (int)x - p * 3 / 2, gy = (int)y - p * 5 / 2;
        glow(fx, x, y, fx->cellw * 1.2f, warm, (int)(80 * (1 - g)));
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < 3; k++)
                if (f->glyph[r * 3 + k] == '1') blob(fx, gx + k * p, gy + r * p, p, p, c, 255);
        return;
    }
    float u = f->glyph ? (tau - GLYPH_T) / (f->life - GLYPH_T) : tau / f->life;
    int sz = u < 0.3f ? 2 * p : p;
    uint32_t c = mixc(f->col, warm, u * 2.5f);
    if (f->firefly) glow(fx, x, y, fx->cellw * 1.4f, warm, (int)(110 * fade));
    blob(fx, (int)x - sz / 2, (int)y - sz / 2, sz, sz, c, (int)(255 * fade));
}

void termfx_render(TermFx *fx, bool outro, float t, const Canvas *world)
{
    memset(fx->buf, 0, (size_t)fx->W * fx->H * 4);
    /* cells covered by the world (or by the terminal background in the margins) */
    for (int r = 0; r < fx->rows; r++)
        for (int c = 0; c < fx->cols; c++) {
            float w = fx->when[r * fx->cols + c];
            float a = outro ? 1 - (t - w) / LAND_FADE : (t - w) / COVER_FADE;
            if (a <= 0) continue;
            int al = a >= 1 ? 255 : (int)(a * 255);
            for (int y = r * fx->cellh; y < (r + 1) * fx->cellh; y++) {
                uint32_t *d = fx->buf + (size_t)y * fx->W;
                int wy = (y - fx->oy) / fx->s;
                bool rowin = y >= fx->oy && wy < world->h;
                for (int x = c * fx->cellw; x < (c + 1) * fx->cellw; x++) {
                    int wx = (x - fx->ox) / fx->s;
                    uint32_t px = rowin && x >= fx->ox && wx < world->w ? world->px[wy * world->w + wx] : fx->bg;
                    d[x] = (uint32_t)al << 24 | (px & 0xffffff);
                }
            }
        }
    /* the cursor glows while the terminal waits for its first spark */
    float cxp = (fx->cur_c + 0.5f) * fx->cellw, cyp = (fx->cur_r + 0.5f) * fx->cellh;
    if (!outro && t < HOLD + 0.1f)
        glow(fx, cxp, cyp, fx->cellw * 2.0f, 0xffd27a, (int)(90 * (0.5f + 0.5f * sinf(t * 18))));
    for (int i = 0; i < fx->nfrag; i++) {
        const struct Frag *f = &fx->frag[i];
        draw_frag(fx, f, outro ? f->when - t : t - f->when);
    }
}
