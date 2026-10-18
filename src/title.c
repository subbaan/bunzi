#include "title.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "game.h"
#include "sound.h"
#include "version.h"

#define NFLIES   22
#define LOGO_Y   12
#define CAT_X    140        /* where the bunny sits: the lit bend of the path */
#define CAT_Y    238
#define MENU_Y   140
#define MENU_DY  18
#define NITEMS   4

enum { CAT_WAIT, CAT_WALK, CAT_SIT, CAT_LEAVE };

typedef struct { float x, y, ph, sp, warm; bool front; } Fly;

struct Title {
    Image sky, lake, front, logo;
    Atlas hero;             /* the shared atlas, for the bunny */
    const Sprite *idle[2], *run[6];
    int moon_x, moon_y, lamp_x, lamp_y;     /* light spots found in the layers */
    int logo_lamp_x, logo_lamp_y;           /* the logo's own lantern */
    float t, et;                            /* scenery time, entrance time */
    bool hold, backdrop;
    int sel, level;         /* menu item, level shown in the menu */
    int cat;
    float cat_x, cat_anim, cat_still, leave_t;
    float sel_t;
    Fly fly[NFLIES];
};

/* ---------------------------------------------------------------- setup */

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float rnd(uint32_t h) { return (h & 0xffff) / 65535.0f; }

static int load(Image *im, const char *dir, const char *file, char *err, size_t errn)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    if (image_load(im, p)) { snprintf(err, errn, "cannot load %.200s", p); return -1; }
    return 0;
}

/* centre of the pixels in a box that pass a colour test */
static void find_light(const Image *im, int x0, int y0, int x1, int y1, bool warm, int *ox, int *oy)
{
    long sx = 0, sy = 0, n = 0;
    for (int y = y0; y < y1 && y < im->h; y++)
        for (int x = x0; x < x1 && x < im->w; x++) {
            uint32_t c = im->px[y * im->w + x];
            int a = (int)(c >> 24), r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
            bool hit = a > 128 && (warm ? r > 240 && g > 170 && b < 160 : r > 225 && g > 225 && b > 200);
            if (hit) { sx += x; sy += y; n++; }
        }
    *ox = n ? (int)(sx / n) : -1000;
    *oy = n ? (int)(sy / n) : -1000;
}

Title *title_create(const char *asset_dir, char *err, size_t errn)
{
    Title *t = calloc(1, sizeof *t);
    if (!t) { snprintf(err, errn, "out of memory"); return NULL; }
    char dir[1300], a[1400], b[1400];
    snprintf(dir, sizeof dir, "%s/title/gen", asset_dir);
    if (load(&t->sky, dir, "bg_sky.png", err, errn) || load(&t->lake, dir, "bg_lake.png", err, errn) ||
        load(&t->front, dir, "bg_front.png", err, errn) || load(&t->logo, dir, "logo.png", err, errn))
        goto fail;
    snprintf(a, sizeof a, "%s/moonlit_ruins/gen/sprites.png", asset_dir);
    snprintf(b, sizeof b, "%s/moonlit_ruins/gen/sprites.txt", asset_dir);
    if (atlas_load(&t->hero, a, b)) { snprintf(err, errn, "cannot load %.200s", a); goto fail; }
    char nm[24];
    t->idle[0] = atlas_find(&t->hero, "b_idle0");
    t->idle[1] = atlas_find(&t->hero, "b_idle1");
    bool ok = t->idle[0] && t->idle[1];
    for (int i = 0; i < 6; i++) {
        snprintf(nm, sizeof nm, "b_run%d", i);
        ok &= (t->run[i] = atlas_find(&t->hero, nm)) != NULL;
    }
    if (!ok) {
        snprintf(err, errn, "bunny sprites missing");
        goto fail;
    }
    find_light(&t->sky, 0, 0, t->sky.w / 3, t->sky.h / 2, false, &t->moon_x, &t->moon_y);
    find_light(&t->front, t->front.w * 2 / 3, 0, t->front.w, t->front.h, true, &t->lamp_x, &t->lamp_y);
    find_light(&t->logo, 0, 0, t->logo.w / 5, t->logo.h, true, &t->logo_lamp_x, &t->logo_lamp_y);
    for (int i = 0; i < NFLIES; i++) {
        uint32_t h = hash2(i, 77);
        t->fly[i] = (Fly){ rnd(h) * VIEW_W, 130 + rnd(h >> 5) * 125, rnd(h >> 9) * 6.28f,
                           0.6f + rnd(h >> 13) * 0.8f, rnd(h >> 17), (h >> 21) & 1 };
    }
    t->sel = 0;
    title_reset(t, false);
    return t;
fail:
    title_destroy(t);
    return NULL;
}

void title_destroy(Title *t)
{
    if (!t) return;
    image_free(&t->sky);
    image_free(&t->lake);
    image_free(&t->front);
    image_free(&t->logo);
    atlas_free(&t->hero);
    free(t);
}

void title_reset(Title *t, bool hold)
{
    t->et = 0;
    t->hold = hold;
    t->cat = CAT_WAIT;
    t->cat_x = -24;
    t->cat_anim = t->cat_still = t->leave_t = 0;
    if (t->sel == NITEMS - 1) t->sel = 0;
}

void title_hold(Title *t, bool hold) { t->hold = hold; }

void title_backdrop(Title *t, bool on) { t->backdrop = on; }

/* --------------------------------------------------------------- update */

static bool menu_ready(const Title *t) { return t->et > 1.5f && t->cat != CAT_LEAVE; }

TitleAction title_update(Title *t, const TitleInput *in, float dt, int *level)
{
    static const TitleInput none;
    int dummy = 0;
    if (!in) in = &none;
    if (!level) { dummy = t->level; level = &dummy; }
    t->t += dt;
    t->sel_t += dt;
    if (!t->hold) t->et += dt;
    t->level = *level;

    bool any = in->up || in->down || in->left || in->right || in->confirm || in->back;
    switch (t->cat) {
    case CAT_WAIT:
        if (t->et > 1.2f) t->cat = CAT_WALK;
        break;
    case CAT_WALK: {        /* trots in and slows to a stop */
        float sp = 30.0f + fminf(1.0f, (CAT_X - t->cat_x) / 60.0f) * 60.0f;
        t->cat_x += sp * dt;
        t->cat_anim += sp * dt;
        if (t->cat_x >= CAT_X) { t->cat_x = CAT_X; t->cat = CAT_SIT; t->cat_still = 0; }
        break;
    }
    case CAT_SIT:
        t->cat_still = any ? 0 : t->cat_still + dt;
        break;
    case CAT_LEAVE: {       /* off on the adventure */
        t->leave_t += dt;
        float sp = 60.0f + t->leave_t * 260.0f;
        t->cat_x += sp * dt;
        t->cat_anim += sp * dt;
        if (t->cat_x > VIEW_W + 30 || t->leave_t > 1.6f) return TA_START;
        return TA_NONE;
    }
    }
    if (!menu_ready(t)) {
        if (any && t->et > 0.1f && !t->hold) {      /* a key hurries the entrance along */
            t->et = 1.5f;
            if (t->cat != CAT_SIT) { t->cat = CAT_SIT; t->cat_x = CAT_X; }
        }
        return TA_NONE;
    }
    int nl = game_level_count();
    if (in->up)   { t->sel = (t->sel + NITEMS - 1) % NITEMS; t->sel_t = 0; snd_play(SFX_MENU, 1.0f, 1.0f, 0); }
    if (in->down) { t->sel = (t->sel + 1) % NITEMS; t->sel_t = 0; snd_play(SFX_MENU, 1.0f, 1.0f, 0); }
    if (t->sel == 1 && (in->left || in->right)) {
        *level = t->level = (*level + (in->right ? 1 : nl - 1)) % nl;
        snd_play(SFX_MENU, 1.0f, in->right ? 1.12f : 0.9f, 0);
    }
    if (in->back) {
        if (t->sel != NITEMS - 1) { t->sel = NITEMS - 1; t->sel_t = 0; snd_play(SFX_MENU, 1.0f, 0.8f, 0); }
        else return TA_QUIT;
    }
    if (in->confirm) {
        if (t->sel == NITEMS - 1) return TA_QUIT;
        if (t->sel == 2) { snd_play(SFX_SELECT, 1.0f, 1.0f, 0); return TA_CONTROLS; }
        if (t->sel == 1) { t->sel = 0; t->sel_t = 0; snd_play(SFX_MENU, 1.0f, 1.0f, 0); return TA_NONE; }
        snd_play(SFX_SELECT, 1.0f, 1.0f, 0);
        t->cat = CAT_LEAVE;
        t->leave_t = 0;
    }
    return TA_NONE;
}

/* --------------------------------------------------------------- render */

static float ease_out_back(float u)
{
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    const float c = 1.6f;
    float v = u - 1;
    return 1 + (c + 1) * v * v * v + c * v * v;
}

static void draw_logo(Title *t, Canvas *cv, int x0, int y0)
{
    const Image *im = &t->logo;
    /* a sheen sweeps across every seven seconds */
    float ph = fmodf(t->t, 7.0f) / 1.3f;
    float band = ph < 1 ? -60 + ph * (im->w + 120) : -1000;
    for (int j = 0; j < im->h; j++) {
        int y = y0 + j;
        if ((unsigned)y >= (unsigned)cv->h) continue;
        for (int i = 0; i < im->w; i++) {
            int x = x0 + i;
            if ((unsigned)x >= (unsigned)cv->w) continue;
            uint32_t c = im->px[j * im->w + i];
            if ((c >> 24) < 128) continue;
            c &= 0xffffff;
            float d = fabsf(i + j * 0.6f - band);
            if (d < 7) c = gfx_mix(c, 0xffffff, (int)((7 - d) / 7 * 130));
            cv->px[y * cv->w + x] = c;
        }
    }
}

static void star(Canvas *cv, int x, int y, int r, uint32_t c)
{
    gfx_put(cv, x, y, 0xffffff);
    for (int k = 1; k <= r; k++) {
        gfx_put(cv, x + k, y, c); gfx_put(cv, x - k, y, c);
        gfx_put(cv, x, y + k, c); gfx_put(cv, x, y - k, c);
    }
}

/* a little paw print marking the chosen item */
static void paw(Canvas *cv, int x, int y, uint32_t c)
{
    static const char *P[7] = { ".x.x.", "x.x.x", ".....", ".xxx.", "xxxxx", "xxxxx", ".xxx." };
    for (int j = 0; j < 7; j++)
        for (int i = 0; i < 5; i++)
            if (P[j][i] == 'x') gfx_put(cv, x + i, y + j, c);
}

static void shadow_text(Canvas *cv, int x, int y, int scale, const char *s, uint32_t c)
{
    gfx_text_scaled(cv, x + 1, y + 1, scale, s, 0x0a0618);
    gfx_text_scaled(cv, x, y, scale, s, c);
}

static void draw_flies(Title *t, Canvas *cv, bool front)
{
    for (int i = 0; i < NFLIES; i++) {
        const Fly *f = &t->fly[i];
        if (f->front != front) continue;
        float tt = t->t * f->sp;
        int x = (int)(f->x + sinf(tt * 0.7f + f->ph) * 26 + sinf(tt * 1.9f + f->ph * 2) * 6);
        int y = (int)(f->y + sinf(tt * 0.9f + f->ph * 3) * 12);
        x = ((x % (VIEW_W + 40)) + VIEW_W + 40) % (VIEW_W + 40) - 20;
        float br = 0.5f + 0.5f * sinf(t->t * (1.5f + f->sp) + f->ph * 5);
        br = br * br;
        uint32_t c = f->warm < 0.6f ? 0xffd27a : 0x9af0c8;
        gfx_glow(cv, x, y, front ? 9 : 7, c, (int)(80 * br) + 10);
        if (br > 0.15f) gfx_put(cv, x, y, gfx_mix(c, 0xffffff, 140));
        if (front && br > 0.6f) gfx_put(cv, x + 1, y, c);
    }
}

void title_render(Title *t, Canvas *cv)
{
    float s = sinf(t->t * 0.07f);
    int sky_ox = 12 + (int)(4 * s), lake_ox = 12 + (int)(7 * s), front_ox = 12 + (int)(11 * s);
    gfx_fill(cv, 0, 0, cv->w, cv->h, t->sky.px[0] & 0xffffff);
    draw_layer(cv, &t->sky, sky_ox, 45, LAYER_EXTEND, 0, 0);
    /* twinkles in the upper sky and a breath of moonlight */
    for (int i = 0; i < 14; i++) {
        uint32_t h = hash2(i, 991);
        int x = (int)(rnd(h) * VIEW_W), y = 4 + (int)(rnd(h >> 7) * 70);
        float br = sinf(t->t * (0.8f + rnd(h >> 3)) + i * 1.7f);
        if (br > 0.75f) star(cv, x, y, br > 0.93f ? 2 : 1, 0x8a90d8);
    }
    if (t->moon_x > -1000)
        gfx_glow(cv, t->moon_x - sky_ox, t->moon_y - 45, 30, 0x8088d0, (int)(40 + 10 * sinf(t->t * 0.6f)));
    draw_layer(cv, &t->lake, lake_ox, -40, LAYER_ALPHA, 0, 0);
    draw_flies(t, cv, false);
    draw_layer(cv, &t->front, front_ox, 8, LAYER_ALPHA, 0, 0);
    if (t->lamp_x > -1000) {
        float fl = 0.85f + 0.15f * sinf(t->t * 13) * sinf(t->t * 7.3f);
        int lx = t->lamp_x - front_ox, ly = t->lamp_y - 8;
        gfx_glow(cv, lx, ly, 46, 0xffa040, (int)(90 * fl));
        gfx_glow(cv, lx, ly, 12, 0xffd080, (int)(110 * fl));
    }

    /* the bunny */
    if (t->cat != CAT_WAIT && !t->backdrop) {
        const Sprite *sp;
        if (t->cat == CAT_WALK || t->cat == CAT_LEAVE) sp = t->run[(int)(t->cat_anim / 9.0f) % 6];
        else sp = fmodf(t->cat_still, 3.4f) > 3.2f ? t->idle[1] : t->idle[0];
        int x = (int)t->cat_x + (front_ox - 12);     /* rides on the foreground */
        draw_sprite(cv, sp, x, CAT_Y, 0, 0, 0);
    }
    draw_flies(t, cv, true);
    if (t->backdrop) return;

    /* logo: drops in and settles, then bobs */
    float u = (t->et - 0.2f) / 0.9f;
    int ly = (int)(-100 + (LOGO_Y + 100) * ease_out_back(u));
    if (u >= 1) ly = LOGO_Y + (int)lroundf(sinf(t->t * 1.3f) * 1.5f);
    if (u > 0) {
        int lx = (cv->w - t->logo.w) / 2;
        draw_logo(t, cv, lx, ly);
        if (t->logo_lamp_x > -1000) {           /* its lantern flickers like the real one */
            float fl = 0.85f + 0.15f * sinf(t->t * 11 + 1.3f) * sinf(t->t * 6.1f);
            gfx_glow(cv, lx + t->logo_lamp_x, ly + t->logo_lamp_y, 22, 0xffa040, (int)(70 * fl));
            gfx_glow(cv, lx + t->logo_lamp_x, ly + t->logo_lamp_y, 7, 0xffd080, (int)(90 * fl));
        }
        if (u >= 1)     /* a glint now and then on the logo */
            for (int k = 0; k < 2; k++) {
                int cyc = (int)(t->t * 0.8f + k * 0.5f);
                uint32_t h = hash2(cyc, 31 + k);
                float ph = fmodf(t->t * 0.8f + k * 0.5f, 1.0f);
                int gx = lx + 20 + (int)(rnd(h) * (t->logo.w - 40)), gy = ly + 10 + (int)(rnd(h >> 8) * (t->logo.h - 30));
                int r = ph < 0.5f ? (int)(ph * 8) : (int)((1 - ph) * 8);
                if (r > 0) star(cv, gx, gy, r, 0xffe8a0);
            }
    }

    /* menu */
    if (t->et > 1.5f) {
        int px = 112, pw = VIEW_W - 2 * px, py = MENU_Y - 8, ph = NITEMS * MENU_DY + 10;
        gfx_tint(cv, px + 2, py, pw - 4, ph, 0x0a0820, 150);
        gfx_tint(cv, px, py + 2, 2, ph - 4, 0x0a0820, 150);
        gfx_tint(cv, px + pw - 2, py + 2, 2, ph - 4, 0x0a0820, 150);
        char buf[64];
        for (int i = 0; i < NITEMS; i++) {
            bool on = i == t->sel && t->cat != CAT_LEAVE;
            if (i == 0) snprintf(buf, sizeof buf, "START GAME");
            else if (i == 1) snprintf(buf, sizeof buf, "%sLEVEL %d  %s%s", on ? "< " : "", t->level + 1,
                                      game_level_title(t->level), on ? " >" : "");
            else if (i == 2) snprintf(buf, sizeof buf, "CONTROLS");
            else snprintf(buf, sizeof buf, "QUIT");
            int w = gfx_text_width(buf, 2), x = (VIEW_W - w) / 2, y = MENU_Y + i * MENU_DY;
            uint32_t c = on ? 0xffd860 : 0xc8c0e8;
            if (t->cat == CAT_LEAVE && i == 0) c = ((int)(t->leave_t * 12) & 1) ? 0xffffff : 0xffd860;
            if (on) {
                float pu = 0.5f + 0.5f * sinf(t->sel_t * 5);
                gfx_glow(cv, VIEW_W / 2, y + 5, w / 2 + 16, 0xffb040, (int)(25 + 20 * pu));
                paw(cv, x - 14, y + 1, 0xffd860);
                paw(cv, x + w + 8, y + 1, 0xffd860);
            }
            shadow_text(cv, x, y, 2, buf, c);
        }
        const char *hint = "ARROWS / D-PAD CHOOSE    Z / ENTER / A SELECT    ESC QUIT";
        int hw = gfx_text_width(hint, 1);
        gfx_text(cv, (VIEW_W - hw) / 2 + 1, VIEW_H - 9 + 1, hint, 0x0a0618);
        gfx_text(cv, (VIEW_W - hw) / 2, VIEW_H - 9, hint, 0xa8a0d0);
    }
    char ver[32];
    snprintf(ver, sizeof ver, "V%s", PLAT_VERSION);
    gfx_text(cv, VIEW_W - gfx_text_width(ver, 1) - 4 + 1, VIEW_H - 9 + 1, ver, 0x0a0618);
    gfx_text(cv, VIEW_W - gfx_text_width(ver, 1) - 4, VIEW_H - 9, ver, 0x8880b0);
}
