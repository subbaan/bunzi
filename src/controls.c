/* Keyboard bindings and the controls screen.
 *
 * Each action has up to four keys. Giving a key to one action takes it from
 * any other (the two swap keys where they can), and no action is ever left
 * without a key. The file is rewritten through a temporary and a rename after
 * every change. */
#include "controls.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "assets.h"
#include "sound.h"
#include "term.h"

static const char *const CT_LABEL[CT_COUNT] = { "LEFT", "RIGHT", "DOWN", "JUMP", "RUN", "RESTART" };
static const char *const CT_FILE[CT_COUNT]  = { "left", "right", "down", "jump", "run", "restart" };
static const char *const CT_HELP[CT_COUNT]  = {
    "RUN LEFT", "RUN RIGHT", "DOWN + JUMP DROPS THROUGH A PLATFORM",
    "HOLD IT FOR A HIGHER JUMP", "HOLD IT TO RUN FASTER", "START THE LEVEL AGAIN",
};
static const int DEFAULTS[CT_COUNT][CT_KEYS] = {
    { K_LEFT, 'a' }, { K_RIGHT, 'd' }, { K_DOWN, 's' }, { 'z', ' ', K_UP, 'w' }, { 'x', K_SHIFT }, { 'r' },
};
static int keys[CT_COUNT][CT_KEYS];     /* 0: empty slot */
static char cfg_dir[1024], cfg_path[1100];

/* ---------------------------------------------------------------- keys */

static const struct { int key; const char *name; } NAMED[] = {
    { ' ', "SPACE" }, { K_TAB, "TAB" }, { K_ENTER, "ENTER" }, { 127, "BKSP" },
    { K_LEFT, "LEFT" }, { K_RIGHT, "RIGHT" }, { K_UP, "UP" }, { K_DOWN, "DOWN" },
    { K_SHIFT, "SHIFT" }, { K_CTRL, "CTRL" }, { K_ALT, "ALT" },
    /* keys the 3x5 font has no glyph for */
    { '"', "DQUOTE" }, { '#', "HASH" }, { '$', "DOLLAR" }, { '&', "AMP" }, { '\'', "QUOTE" },
    { '*', "STAR" }, { ';', "SEMI" }, { '@', "AT" }, { '\\', "BSLASH" }, { '`', "GRAVE" },
    { '{', "LBRACE" }, { '|', "PIPE" }, { '}', "RBRACE" }, { '~', "TILDE" },
};
#define NNAMED (int)(sizeof NAMED / sizeof NAMED[0])

/* Esc always means back, so it is never bound */
static bool bindable(int key)
{
    return (key > ' ' && key < 127 && !(key >= 'A' && key <= 'Z')) || key == ' ' || key == K_TAB ||
           key == K_ENTER || key == 127 || (key >= K_LEFT && key <= K_ALT);
}

const char *ctl_key_name(int key)
{
    for (int i = 0; i < NNAMED; i++)
        if (NAMED[i].key == key) return NAMED[i].name;
    static char b[8];
    if (key > ' ' && key < 127) snprintf(b, sizeof b, "%c", key >= 'a' && key <= 'z' ? key - 32 : key);
    else snprintf(b, sizeof b, "K%d", key);
    return b;
}

static int key_parse(const char *s)
{
    for (int i = 0; i < NNAMED; i++)
        if (!strcmp(NAMED[i].name, s)) return NAMED[i].key;
    if (s[0] && !s[1]) {
        int k = (unsigned char)s[0];
        if (k >= 'A' && k <= 'Z') k += 32;
        return bindable(k) ? k : 0;
    }
    return 0;
}

bool ctl_has(int key, Control c)
{
    for (int i = 0; i < CT_KEYS; i++)
        if (keys[c][i] == key) return key != 0;
    return false;
}

bool ctl_bound(int key)
{
    for (int c = 0; c < CT_COUNT; c++)
        if (ctl_has(key, (Control)c)) return true;
    return false;
}

bool ctl_any(const bool *k, Control c)
{
    for (int i = 0; i < CT_KEYS; i++)
        if (keys[c][i] && k[keys[c][i]]) return true;
    return false;
}

static int count_keys(int c)
{
    int n = 0;
    for (int i = 0; i < CT_KEYS; i++) n += keys[c][i] != 0;
    return n;
}

/* ---------------------------------------------------------------- file */

static void find_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') snprintf(cfg_dir, sizeof cfg_dir, "%s/bunzi", xdg);
    else if (home && home[0]) snprintf(cfg_dir, sizeof cfg_dir, "%s/.config/bunzi", home);
    else { cfg_dir[0] = cfg_path[0] = 0; return; }
    snprintf(cfg_path, sizeof cfg_path, "%s/controls.txt", cfg_dir);
}

void ctl_load(void)
{
    memcpy(keys, DEFAULTS, sizeof keys);
    find_path();
    FILE *f = cfg_path[0] ? fopen(cfg_path, "r") : NULL;
    if (!f) return;
    int got[CT_COUNT][CT_KEYS] = { { 0 } };
    bool have[CT_COUNT] = { 0 };
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *sv, *tok = strtok_r(line, " \t\r\n", &sv);
        if (!tok || tok[0] == '#') continue;
        int c = 0;
        while (c < CT_COUNT && strcmp(tok, CT_FILE[c])) c++;
        if (c == CT_COUNT || have[c]) continue;
        int n = 0;
        while (n < CT_KEYS && (tok = strtok_r(NULL, " \t\r\n", &sv))) {
            int k = key_parse(tok);
            bool dup = !k;
            for (int a = 0; a < CT_COUNT && !dup; a++)
                for (int i = 0; i < CT_KEYS; i++) dup |= got[a][i] == k;
            if (!dup) got[c][n++] = k;
        }
        have[c] = n > 0;
    }
    fclose(f);
    for (int c = 0; c < CT_COUNT; c++)
        if (have[c]) memcpy(keys[c], got[c], sizeof keys[c]);
    /* an action the file left out keeps its defaults, less any key now used elsewhere */
    for (int c = 0; c < CT_COUNT; c++) {
        if (have[c]) continue;
        for (int i = 0; i < CT_KEYS; i++)
            for (int a = 0; a < CT_COUNT; a++)
                if (a != c && have[a] && ctl_has(keys[c][i], (Control)a)) keys[c][i] = 0;
    }
}

static int ctl_save(void)
{
    if (!cfg_path[0]) return -1;
    /* the config directory, one level at a time (~/.config may not exist yet) */
    char p[1024];
    snprintf(p, sizeof p, "%s", cfg_dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    if (mkdir(p, 0755) && errno != EEXIST) return -1;

    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", cfg_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# bunzi controls: an action, then up to %d keys (change them on the title screen)\n", CT_KEYS);
    for (int c = 0; c < CT_COUNT; c++) {
        fprintf(f, "%s", CT_FILE[c]);
        for (int i = 0; i < CT_KEYS; i++)
            if (keys[c][i]) fprintf(f, " %s", ctl_key_name(keys[c][i]));
        fputc('\n', f);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, cfg_path)) { unlink(tmp); return -1; }
    return 0;
}

/* -------------------------------------------------------------- screen */

#define FRAME_X   3             /* the frame is drawn at 2x */
#define FRAME_Y   1
#define IN_X0     33            /* its opening */
#define IN_Y0     31
#define IN_X1     447
#define IN_Y1     245
#define ROW_Y     68
#define ROW_DY    18
#define LABEL_X   60
#define SLOT_X    140
#define SLOT_DX   72
#define SLOT_W    66
#define SLOT_H    16
#define GROUND_Y  228           /* the demo stage */
#define STAGE_X0  44
#define STAGE_X1  250
#define BTN_X     360
#define NROWS     (CT_COUNT + 1)    /* the last row: DEFAULTS / DONE */

static const uint32_t INK = 0x10102a, GOLD = 0xffd860, DIM = 0xa8a0d0, WHITE = 0xf4f0e0, PINK = 0xff8aa0,
                      FACE = 0xf2ead6, FACE_HI = 0xfffbef, LIP = 0xb4a487, KEYTXT = 0x2c2450,
                      HOLE = 0x5a5288;

struct CtlScreen {
    Image frame;
    Atlas hero;
    const Sprite *idle[2], *run[6], *jump, *fall, *land, *dash[3], *start, *alert, *ground;
    const Sprite *decor[4];
    const Sprite *puff[3], *dust[4];
    int row, col, bcol;
    bool listening;
    float t, sel_t, demo_t, listen_t;
    char msg[64];
    uint32_t msg_c;
    float msg_t;
    float flash[CT_COUNT][CT_KEYS];
    bool held[K_MAX];
    bool save_failed;
};

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

CtlScreen *ctl_screen_create(const char *asset_dir, char *err, size_t errn)
{
    CtlScreen *s = calloc(1, sizeof *s);
    if (!s) { snprintf(err, errn, "out of memory"); return NULL; }
    char a[1400], b[1400];
    snprintf(a, sizeof a, "%s/controls/gen/frame.png", asset_dir);
    if (image_load(&s->frame, a)) { snprintf(err, errn, "cannot load %.200s", a); goto fail; }
    snprintf(a, sizeof a, "%s/moonlit_ruins/gen/sprites.png", asset_dir);
    snprintf(b, sizeof b, "%s/moonlit_ruins/gen/sprites.txt", asset_dir);
    if (atlas_load(&s->hero, a, b)) { snprintf(err, errn, "cannot load %.200s", a); goto fail; }
    const Atlas *h = &s->hero;
    char nm[24];
    bool ok = true;
    for (int i = 0; i < 6; i++) {
        snprintf(nm, sizeof nm, "b_run%d", i);
        ok &= (s->run[i] = atlas_find(h, nm)) != NULL;
    }
    for (int i = 0; i < 3; i++) {
        snprintf(nm, sizeof nm, "b_dash%d", i);
        ok &= (s->dash[i] = atlas_find(h, nm)) != NULL;
        snprintf(nm, sizeof nm, "puff%d", i);
        ok &= (s->puff[i] = atlas_find(h, nm)) != NULL;
    }
    for (int i = 0; i < 4; i++) {
        snprintf(nm, sizeof nm, "dust%d", i);
        ok &= (s->dust[i] = atlas_find(h, nm)) != NULL;
    }
    ok &= (s->idle[0] = atlas_find(h, "b_idle0")) && (s->idle[1] = atlas_find(h, "b_idle1")) &&
          (s->jump = atlas_find(h, "b_jump")) && (s->fall = atlas_find(h, "b_fall0")) &&
          (s->land = atlas_find(h, "b_land")) && (s->start = atlas_find(h, "b_start")) &&
          (s->alert = atlas_find(h, "b_alert")) && (s->ground = atlas_find(h, "top0")) &&
          (s->decor[0] = atlas_find(h, "grass0")) &&
          (s->decor[1] = atlas_find(h, "flower0")) && (s->decor[2] = atlas_find(h, "grass1")) &&
          (s->decor[3] = atlas_find(h, "flower2"));
    if (!ok) { snprintf(err, errn, "controls screen: sprites missing"); goto fail; }
    s->bcol = 1;
    return s;
fail:
    ctl_screen_destroy(s);
    return NULL;
}

void ctl_screen_destroy(CtlScreen *s)
{
    if (!s) return;
    image_free(&s->frame);
    atlas_free(&s->hero);
    free(s);
}

void ctl_screen_open(CtlScreen *s)
{
    s->row = s->col = 0;
    s->bcol = 1;
    s->listening = false;
    s->t = s->sel_t = s->demo_t = 0;
    s->msg_t = 0;
    memset(s->flash, 0, sizeof s->flash);
    memset(s->held, 0, sizeof s->held);
}

void ctl_screen_select(CtlScreen *s, int row, bool listening)
{
    s->row = row < 0 ? 0 : row > CT_COUNT ? CT_COUNT : row;
    s->listening = listening && s->row < CT_COUNT;
}

static void say(CtlScreen *s, uint32_t c, const char *fmt, const char *a, const char *b)
{
    snprintf(s->msg, sizeof s->msg, fmt, a, b);
    s->msg_c = c;
    s->msg_t = 2.5f;
}

static void saved(CtlScreen *s)
{
    s->save_failed = ctl_save() != 0;
}

/* a hotkey this key used to be, now that an action has it */
static const char *shortcut(int key)
{
    switch (key) {
    case 'n': return "MUTE";
    case 'q': return "LEAVE THE LEVEL";
    case 'm': case 'f': case 'g': return "DEBUG";
    case K_TAB: return "OVERLAY";
    }
    return key >= '1' && key <= '9' ? "LEVEL JUMP" : NULL;
}

static void assign(CtlScreen *s, int key)
{
    int r = s->row, c = s->col, old = keys[r][c];
    char kn[16];
    snprintf(kn, sizeof kn, "%s", ctl_key_name(key));
    if (old == key) { s->flash[r][c] = 1; snd_play(SFX_SELECT, 1.0f, 1.0f, 0); return; }
    s->msg_t = 0;
    for (int r2 = 0; r2 < CT_COUNT; r2++)
        for (int c2 = 0; c2 < CT_KEYS; c2++) {
            if (keys[r2][c2] != key) continue;
            if (r2 != r && !old && count_keys(r2) == 1) {
                say(s, PINK, "%s IS THE ONLY %s KEY", kn, CT_LABEL[r2]);
                snd_play(SFX_MENU, 1.0f, 0.7f, 0);
                return;
            }
            keys[r2][c2] = old;     /* a swap, or a move within the row */
            s->flash[r2][c2] = 1;
            if (r2 != r && old) say(s, GOLD, "SWAPPED: %s NOW HAS %s", CT_LABEL[r2], ctl_key_name(old));
            else if (r2 != r) say(s, GOLD, "%s TAKEN FROM %s", kn, CT_LABEL[r2]);
        }
    keys[r][c] = key;
    s->flash[r][c] = 1;
    const char *sc = shortcut(key);
    if (sc && s->msg_t <= 0) say(s, DIM, "%s IS NO LONGER THE %s KEY", kn, sc);
    snd_play(SFX_SELECT, 1.0f, 1.0f, 0);
    saved(s);
}

bool ctl_screen_update(CtlScreen *s, const CtlNav *in, const bool *pressed, const bool *down, float dt)
{
    s->t += dt;
    s->sel_t += dt;
    s->demo_t += dt;
    if (s->msg_t > 0) s->msg_t -= dt;
    for (int r = 0; r < CT_COUNT; r++)
        for (int c = 0; c < CT_KEYS; c++)
            if (s->flash[r][c] > 0) s->flash[r][c] = fmaxf(0, s->flash[r][c] - dt * 1.6f);
    if (!in) return false;
    if (down) memcpy(s->held, down, sizeof s->held);
    if (s->listening) {
        s->listen_t += dt;
        if (in->back) {
            s->listening = false;
            snd_play(SFX_MENU, 1.0f, 0.8f, 0);
            return false;
        }
        for (int k = 0; k < K_MAX && pressed; k++)
            if (pressed[k] && bindable(k)) {
                s->listening = false;
                assign(s, k);
                break;
            }
        return false;
    }
    if (in->back) { snd_play(SFX_MENU, 1.0f, 0.8f, 0); return true; }
    if (in->up || in->down) {
        s->row = (s->row + (in->up ? NROWS - 1 : 1)) % NROWS;
        s->sel_t = s->demo_t = 0;
        snd_play(SFX_MENU, 1.0f, 1.0f, 0);
    }
    if (s->row < CT_COUNT) {
        if (in->left || in->right) {
            s->col = (s->col + (in->left ? CT_KEYS - 1 : 1)) % CT_KEYS;
            s->sel_t = 0;
            snd_play(SFX_MENU, 1.0f, in->right ? 1.08f : 0.94f, 0);
        }
        if (in->confirm) {
            s->listening = true;
            s->listen_t = 0;
            s->msg_t = 0;
            snd_play(SFX_SELECT, 1.0f, 1.2f, 0);
        } else if (in->clear && keys[s->row][s->col]) {
            if (count_keys(s->row) == 1) {
                say(s, PINK, "%s NEEDS A KEY%s", CT_LABEL[s->row], "");
                snd_play(SFX_MENU, 1.0f, 0.7f, 0);
            } else {
                keys[s->row][s->col] = 0;
                snd_play(SFX_MENU, 1.0f, 0.85f, 0);
                saved(s);
            }
        }
    } else {
        if (in->left || in->right) { s->bcol ^= 1; snd_play(SFX_MENU, 1.0f, 1.0f, 0); }
        if (in->confirm) {
            if (s->bcol) { snd_play(SFX_SELECT, 1.0f, 1.0f, 0); return true; }
            memcpy(keys, DEFAULTS, sizeof keys);
            for (int r = 0; r < CT_COUNT; r++)
                for (int c = 0; c < CT_KEYS; c++) s->flash[r][c] = keys[r][c] ? 1 : 0;
            say(s, GOLD, "DEFAULTS RESTORED%s%s", "", "");
            snd_play(SFX_RECORD, 0.8f, 1.0f, 0);
            saved(s);
        }
    }
    return false;
}

/* ------------------------------------------------------------- drawing */

static void txt(Canvas *cv, int x, int y, int sc, const char *m, uint32_t c)
{
    int o = sc >= 3 ? 2 : 1;
    gfx_text_scaled(cv, x + o, y + o, sc, m, INK);
    gfx_text_scaled(cv, x, y, sc, m, c);
}

static void star(Canvas *cv, int x, int y, int r, uint32_t c)
{
    gfx_put(cv, x, y, 0xffffff);
    for (int k = 1; k <= r; k++) {
        gfx_put(cv, x + k, y, c); gfx_put(cv, x - k, y, c);
        gfx_put(cv, x, y + k, c); gfx_put(cv, x, y - k, c);
    }
}

static void paw(Canvas *cv, int x, int y, uint32_t c)
{
    static const char *P[7] = { ".x.x.", "x.x.x", ".....", ".xxx.", "xxxxx", "xxxxx", ".xxx." };
    for (int j = 0; j < 7; j++)
        for (int i = 0; i < 5; i++)
            if (P[j][i] == 'x') gfx_put(cv, x + i, y + j, c);
}

static void frame_blit(Canvas *cv, const Image *im, int x0, int y0)
{
    for (int j = 0; j < im->h * 2; j++) {
        int y = y0 + j;
        if ((unsigned)y >= (unsigned)cv->h) continue;
        const uint32_t *src = im->px + (size_t)(j / 2) * im->w;
        for (int i = 0; i < im->w * 2; i++) {
            int x = x0 + i;
            uint32_t c = src[i / 2];
            if ((c >> 24) >= 128 && (unsigned)x < (unsigned)cv->w) cv->px[y * cv->w + x] = c & 0xffffff;
        }
    }
}

/* arrows for the arrow keys, 5x5 drawn at 2x */
static void arrow(Canvas *cv, int x, int y, int key, uint32_t c)
{
    static const char *A[5] = { "..x..", ".xxx.", "x.x.x", "..x..", "..x.." };
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 5; i++) {
            int u = i, v = j;               /* the up arrow, turned */
            if (key == K_DOWN) v = 4 - j;
            else if (key == K_LEFT) { u = j; v = i; }
            else if (key == K_RIGHT) { u = j; v = 4 - i; }
            if (A[v][u] == 'x') gfx_fill(cv, x + i * 2, y + j * 2, 2, 2, c);
        }
}

static void rrect(Canvas *cv, int x, int y, int w, int h, uint32_t c)
{
    gfx_fill(cv, x + 1, y, w - 2, h, c);
    gfx_fill(cv, x, y + 1, 1, h - 2, c);
    gfx_fill(cv, x + w - 1, y + 1, 1, h - 2, c);
}

static void keycap(Canvas *cv, int x, int y, int key, bool sel, bool listen, bool held, float flash, float t)
{
    int w = SLOT_W, h = SLOT_H;
    if (sel) {
        float pu = 0.5f + 0.5f * sinf(t * 6);
        gfx_glow(cv, x + w / 2, y + h / 2, w / 2 + 8, 0xffb040, (int)(30 + 25 * pu));
    }
    if (listen) {
        float pu = 0.5f + 0.5f * sinf(t * 9);
        rrect(cv, x - 1, y - 1, w + 2, h + 2, GOLD);
        rrect(cv, x, y, w, h, INK);
        rrect(cv, x + 1, y + 1, w - 2, h - 2, gfx_mix(0x6a5420, GOLD, (int)(90 + 120 * pu)));
        gfx_text_scaled(cv, x + (w - 6) / 2, y + 3, 2, "?", ((int)(t * 3) & 1) ? WHITE : INK);
        return;
    }
    if (!key) {                             /* an empty slot: a dotted outline */
        uint32_t c = sel ? GOLD : HOLE;
        for (int i = 2; i < w - 2; i += 3) { gfx_put(cv, x + i, y + 1, c); gfx_put(cv, x + i, y + h - 2, c); }
        for (int j = 3; j < h - 2; j += 3) { gfx_put(cv, x + 1, y + j, c); gfx_put(cv, x + w - 2, y + j, c); }
        if (sel) txt(cv, x + (w - 3 * 2) / 2 - 1, y + 3, 2, "+", GOLD);
        return;
    }
    int p = held ? 2 : 0, lip = held ? 1 : 3;
    if (sel) rrect(cv, x - 1, y - 1 + p, w + 2, h + 2 - p, GOLD);
    rrect(cv, x, y + p, w, h - p, INK);
    uint32_t face = FACE, hi = FACE_HI, lp = LIP;
    if (held) { face = gfx_mix(FACE, GOLD, 110); hi = gfx_mix(FACE_HI, GOLD, 90); }
    if (flash > 0) { int f = (int)(flash * 200); face = gfx_mix(face, 0xffffff, f); lp = gfx_mix(lp, GOLD, f); }
    gfx_fill(cv, x + 1, y + 1 + p, w - 2, h - 2 - p, lp);
    gfx_fill(cv, x + 2, y + 1 + p, w - 4, h - 2 - p - lip, face);
    gfx_fill(cv, x + 3, y + 1 + p, w - 6, 1, hi);
    int fy = y + 1 + p, fh = h - 2 - p - lip;       /* the face, for centring */
    uint32_t tc = KEYTXT;
    if (key >= K_LEFT && key <= K_DOWN) arrow(cv, x + (w - 10) / 2, fy + (fh - 10) / 2 + 1, key, tc);
    else {
        const char *n = ctl_key_name(key);
        gfx_text_scaled(cv, x + (w - gfx_text_width(n, 2)) / 2, fy + (fh - 10) / 2 + 1, 2, n, tc);
    }
    if (flash > 0)                          /* a few sparks fly off a newly set key */
        for (int k = 0; k < 6; k++) {
            uint32_t hh = hash2(k, x * 7 + y);
            float a = (float)(hh & 1023) / 1023.0f * 6.283f, u = 1 - flash;
            int sx = x + w / 2 + (int)(cosf(a) * (w / 2 + 4 + u * 14)), sy = y + h / 2 + (int)(sinf(a) * (8 + u * 10));
            star(cv, sx, sy, flash > 0.5f ? 2 : 1, GOLD);
        }
}

/* the bunny shows what the chosen action does */
static void demo(const CtlScreen *s, Canvas *cv)
{
    sprite_clip(IN_X0, IN_Y0, IN_X1, IN_Y1);
    for (int x = STAGE_X0 + 4; x < STAGE_X1; x += s->ground->w)
        draw_sprite(cv, s->ground, x + s->ground->ax, GROUND_Y + s->ground->h - 1, 0, 0, 0);
    static const int DX[4] = { 18, 74, 150, 188 };      /* a little grass and a few flowers */
    for (int i = 0; i < 4; i++) draw_sprite(cv, s->decor[i], STAGE_X0 + DX[i], GROUND_Y + 2, i & 1 ? SPR_FLIP : 0, 0, 0);
    float u = s->demo_t;
    int mid = (STAGE_X0 + STAGE_X1) / 2, gy = GROUND_Y;
    const Sprite *b = s->idle[fmodf(s->t, 3.4f) > 3.2f];
    int bx = mid, by = gy, fl = 0;
    int row = s->listening ? -1 : s->row;
    switch (row) {
    case CT_LEFT: case CT_RIGHT: {
        float d = fmodf(u * 70, (float)(STAGE_X1 - STAGE_X0 - 30));
        bx = row == CT_RIGHT ? STAGE_X0 + 15 + (int)d : STAGE_X1 - 15 - (int)d;
        b = s->run[(int)(u * 70 / 9) % 6];
        fl = row == CT_LEFT ? SPR_FLIP : 0;
        break;
    }
    case CT_DOWN: {                     /* off the ledge through it, and back up */
        int px = mid + 20, py = gy - 15;
        gfx_fill(cv, px - 16, py, 32, 4, INK);          /* a thin plank, the kind you can drop through */
        gfx_fill(cv, px - 15, py + 1, 30, 2, 0x9a6438);
        gfx_fill(cv, px - 15, py + 1, 30, 1, 0xc8915a);
        for (int k = -8; k <= 8; k += 16) gfx_put(cv, px + k, py + 2, 0x5a3a20);
        float p = fmodf(u, 2.6f);
        bx = px;
        if (p < 0.7f) by = py;
        else if (p < 1.05f) { float q = (p - 0.7f) / 0.35f; by = py + (int)(15 * q * q); b = s->fall; }
        else if (p < 1.3f) b = s->land;
        else if (p < 1.9f) {
            float q = (p - 1.3f) / 0.6f;
            by = gy - (int)(sinf(q * 3.1416f) * 12 + q * 15);
            if (q > 0.85f) by = py;
            b = q < 0.5f ? s->jump : s->fall;
        } else by = py;
        break;
    }
    case CT_JUMP: {
        float p = fmodf(u, 1.3f);
        if (p > 0.3f && p < 1.0f) {
            float q = (p - 0.3f) / 0.7f;
            by = gy - (int)(sinf(q * 3.1416f) * 16);
            b = q < 0.5f ? s->jump : s->fall;
        } else if (p >= 1.0f && p < 1.12f) {
            b = s->land;
            draw_sprite(cv, s->dust[(int)((p - 1.0f) / 0.03f) & 3], bx - 10, gy - 3, 0, 0, 0);
            draw_sprite(cv, s->dust[(int)((p - 1.0f) / 0.03f) & 3], bx + 10, gy - 3, SPR_FLIP, 0, 0);
        }
        break;
    }
    case CT_RUN: {
        float d = fmodf(u * 150, (float)(STAGE_X1 - STAGE_X0 - 40));
        bx = STAGE_X0 + 20 + (int)d;
        b = s->dash[(int)(u * 12) % 3];
        for (int k = 1; k <= 3; k++)
            if (bx - k * 12 > STAGE_X0 + 4)
                draw_sprite(cv, s->dust[(k + (int)(u * 12)) & 3], bx - 8 - k * 12, gy - 4, 0, 0, 0);
        break;
    }
    case CT_RESTART: {                  /* vanishes in a puff, back at the start */
        float p = fmodf(u, 2.4f);
        bx = mid + 40;
        if (p > 0.8f && p < 1.2f) {
            b = NULL;
            draw_sprite(cv, s->puff[(int)((p - 0.8f) / 0.14f) % 3], bx, gy - 10, 0, 0, 0);
        } else if (p >= 1.2f) {
            bx = STAGE_X0 + 30;
            if (p < 1.6f) {
                b = s->start;
                star(cv, bx - 8 + (int)(p * 40) % 16, gy - 26, 2, GOLD);
                star(cv, bx + 8 - (int)(p * 30) % 14, gy - 14, 1, GOLD);
            }
        }
        break;
    }
    default:
        break;
    }
    if (b) draw_sprite(cv, b, bx, by, fl, 0, 0);
    if (s->listening && b)              /* all ears */
        draw_sprite(cv, s->alert, bx + 1, by - b->h - 3 - (int)(fabsf(sinf(s->t * 6)) * 3), 0, 0, 0);
    sprite_noclip();
}

void ctl_screen_render(const CtlScreen *s, Canvas *cv)
{
    float e = fminf(1.0f, s->t / 0.25f);
    gfx_tint(cv, 0, 0, cv->w, cv->h, 0x05061a, (int)(110 * e));
    gfx_tint(cv, IN_X0 - 4, IN_Y0 - 4, IN_X1 - IN_X0 + 8, IN_Y1 - IN_Y0 + 8, 0x0c0a26, (int)(170 * e));
    frame_blit(cv, &s->frame, FRAME_X, FRAME_Y);

    const char *head = "CONTROLS";
    int hw = gfx_text_width(head, 3), hx = (cv->w - hw) / 2;
    txt(cv, hx, 42, 3, head, GOLD);
    paw(cv, hx - 14, 46, GOLD);
    paw(cv, hx + hw + 9, 46, GOLD);
    const char *hint = s->listening ? "PRESS THE NEW KEY  -  ESC CANCELS"
                                    : "ARROWS CHOOSE   ENTER CHANGE   BACKSPACE CLEAR   ESC BACK";
    gfx_text(cv, (cv->w - gfx_text_width(hint, 1)) / 2 + 1, 59, hint, INK);
    gfx_text(cv, (cv->w - gfx_text_width(hint, 1)) / 2, 58, hint, DIM);

    for (int r = 0; r < CT_COUNT; r++) {
        int y = ROW_Y + r * ROW_DY;
        bool on = r == s->row;
        if (on) gfx_tint(cv, LABEL_X - 6, y - 2, SLOT_X + CT_KEYS * SLOT_DX - LABEL_X, SLOT_H + 4, 0xffd860, 22);
        txt(cv, LABEL_X, y + 3, 2, CT_LABEL[r], on ? GOLD : DIM);
        for (int c = 0; c < CT_KEYS; c++) {
            int k = keys[r][c];
            bool sel = on && c == s->col;
            keycap(cv, SLOT_X + c * SLOT_DX, y, k, sel, sel && s->listening, k && s->held[k],
                   s->flash[r][c], s->sel_t);
        }
    }

    /* below: the stage on the left, the buttons on the right */
    demo(s, cv);
    char b[80];
    const char *m;
    uint32_t mc = DIM;
    if (s->listening) {
        snprintf(b, sizeof b, "PRESS A KEY FOR %s", CT_LABEL[s->row]);
        m = b;
        mc = ((int)(s->t * 3) & 1) ? GOLD : WHITE;
    } else if (s->msg_t > 0) { m = s->msg; mc = s->msg_c; }
    else if (s->save_failed) { m = "COULD NOT SAVE THE CONTROLS"; mc = PINK; }
    else if (s->row < CT_COUNT) m = CT_HELP[s->row];
    else m = s->bcol ? "BACK TO THE TITLE" : "PUT EVERY KEY BACK AS IT WAS";
    int sc = s->listening ? 2 : 1;
    txt(cv, (STAGE_X0 + STAGE_X1 - gfx_text_width(m, sc)) / 2, 179, sc, m, mc);

    static const char *const BTN[2] = { "DEFAULTS", "DONE" };
    for (int i = 0; i < 2; i++) {
        bool on = s->row == CT_COUNT && s->bcol == i;
        int w = gfx_text_width(BTN[i], 2), x = BTN_X - w / 2, y = 188 + i * 22;
        if (on) {
            float pu = 0.5f + 0.5f * sinf(s->sel_t * 5);
            gfx_glow(cv, BTN_X, y + 5, w / 2 + 14, 0xffb040, (int)(25 + 20 * pu));
            paw(cv, x - 13, y + 1, GOLD);
            paw(cv, x + w + 7, y + 1, GOLD);
        }
        txt(cv, x, y, 2, BTN[i], on ? GOLD : 0xc8c0e8);
    }
}
