/* Keyboard and gamepad bindings, and the controls screen.
 *
 * Each action has up to four keys and four gamepad buttons. Giving a key to
 * one action takes it from any other (the two swap keys where they can), and
 * no action is ever left without one. The file is rewritten through a
 * temporary and a rename after every change. */
#include "controls.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "assets.h"
#include "pad.h"
#include "sound.h"
#include "term.h"

enum { DEV_KEYS, DEV_PAD, NDEV };

static const char *const CT_LABEL[CT_COUNT] = { "LEFT", "RIGHT", "DOWN", "JUMP", "RUN", "RESTART" };
static const char *const CT_FILE[CT_COUNT]  = { "left", "right", "down", "jump", "run", "restart" };
static const char *const CT_HELP[NDEV][CT_COUNT] = {
    { "RUN LEFT", "RUN RIGHT", "DOWN + JUMP DROPS THROUGH A PLATFORM",
      "HOLD IT FOR A HIGHER JUMP", "HOLD IT TO RUN FASTER", "START THE LEVEL AGAIN" },
    { "RUN LEFT  -  THE LEFT STICK WORKS TOO", "RUN RIGHT  -  THE LEFT STICK WORKS TOO",
      "DOWN + JUMP DROPS THROUGH A PLATFORM", "HOLD IT FOR A HIGHER JUMP", "HOLD IT TO RUN FASTER",
      "START THE LEVEL AGAIN" },
};
static const char *const WORD[NDEV] = { "KEY", "BUTTON" };
static const int DEFAULTS[NDEV][CT_COUNT][CT_KEYS] = {
    { { K_LEFT, 'a' }, { K_RIGHT, 'd' }, { K_DOWN, 's' }, { 'z', ' ', K_UP, 'w' }, { 'x', K_SHIFT }, { 'r' } },
    { { PB_LEFT }, { PB_RIGHT }, { PB_DOWN }, { PB_A }, { PB_X, PB_RB, PB_RT }, { PB_BACK } },
};
static int binds[NDEV][CT_COUNT][CT_KEYS];     /* 0: empty slot */
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

/* the file's names for the buttons */
static const char *const PB_NAME[PB_COUNT] = {
    "", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START",
    "D-UP", "D-DOWN", "D-LEFT", "D-RIGHT", "P1", "P2", "P3", "P4", "MISC",
};
/* On screen the face buttons and the d-pad are pictures, so they read right
 * on any make of pad: \1-\4 the bottom, right, left and top face button,
 * \5-\10 the d-pad's up, down, left and right, \11 the whole d-pad. */
static const char *const PB_LABEL[PB_COUNT] = {
    "", "\1", "\2", "\3", "\4", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START",
    "\5", "\6", "\7", "\10", "P1", "P2", "P3", "P4", "MISC",
};

/* Esc always means back, so it is never bound; nor is the pad's guide button */
static bool bindable(int dev, int key)
{
    if (dev == DEV_PAD) return key > 0 && key < PB_COUNT;
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

const char *ctl_pad_name(int b) { return b > 0 && b < PB_COUNT ? PB_NAME[b] : "?"; }

static const char *label(int dev, int k) { return dev == DEV_PAD ? PB_LABEL[k] : ctl_key_name(k); }

static int key_parse(int dev, const char *s)
{
    if (dev == DEV_PAD) {
        for (int b = 1; b < PB_COUNT; b++)
            if (!strcasecmp(PB_NAME[b], s)) return b;
        return 0;
    }
    for (int i = 0; i < NNAMED; i++)
        if (!strcmp(NAMED[i].name, s)) return NAMED[i].key;
    if (s[0] && !s[1]) {
        int k = (unsigned char)s[0];
        if (k >= 'A' && k <= 'Z') k += 32;
        return bindable(DEV_KEYS, k) ? k : 0;
    }
    return 0;
}

static bool has(int dev, int key, int c)
{
    for (int i = 0; i < CT_KEYS; i++)
        if (binds[dev][c][i] == key) return key != 0;
    return false;
}

static bool bound(int dev, int key)
{
    for (int c = 0; c < CT_COUNT; c++)
        if (has(dev, key, c)) return true;
    return false;
}

static bool any(int dev, const bool *k, int c)
{
    for (int i = 0; i < CT_KEYS; i++)
        if (binds[dev][c][i] && k[binds[dev][c][i]]) return true;
    return false;
}

bool ctl_has(int key, Control c) { return has(DEV_KEYS, key, c); }
bool ctl_bound(int key) { return bound(DEV_KEYS, key); }
bool ctl_any(const bool *k, Control c) { return any(DEV_KEYS, k, c); }
bool ctl_pad_bound(int b) { return bound(DEV_PAD, b); }
bool ctl_pad_any(const bool *btn, Control c) { return any(DEV_PAD, btn, c); }

static int count_keys(int dev, int c)
{
    int n = 0;
    for (int i = 0; i < CT_KEYS; i++) n += binds[dev][c][i] != 0;
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

/* lines are "jump Z SPACE" for keys and "pad-jump A" for buttons */
void ctl_load(void)
{
    memcpy(binds, DEFAULTS, sizeof binds);
    find_path();
    FILE *f = cfg_path[0] ? fopen(cfg_path, "r") : NULL;
    if (!f) return;
    int got[NDEV][CT_COUNT][CT_KEYS] = { { { 0 } } };
    bool have[NDEV][CT_COUNT] = { { 0 } };
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *sv, *tok = strtok_r(line, " \t\r\n", &sv);
        if (!tok || tok[0] == '#') continue;
        int dev = DEV_KEYS;
        if (!strncmp(tok, "pad-", 4)) { dev = DEV_PAD; tok += 4; }
        int c = 0;
        while (c < CT_COUNT && strcmp(tok, CT_FILE[c])) c++;
        if (c == CT_COUNT || have[dev][c]) continue;
        int n = 0;
        while (n < CT_KEYS && (tok = strtok_r(NULL, " \t\r\n", &sv))) {
            int k = key_parse(dev, tok);
            bool dup = !k;
            for (int a = 0; a < CT_COUNT && !dup; a++)
                for (int i = 0; i < CT_KEYS; i++) dup |= got[dev][a][i] == k;
            if (!dup) got[dev][c][n++] = k;
        }
        have[dev][c] = n > 0;
    }
    fclose(f);
    for (int d = 0; d < NDEV; d++) {
        for (int c = 0; c < CT_COUNT; c++)
            if (have[d][c]) memcpy(binds[d][c], got[d][c], sizeof binds[d][c]);
        /* an action the file left out keeps its defaults, less any key now used elsewhere */
        for (int c = 0; c < CT_COUNT; c++) {
            if (have[d][c]) continue;
            for (int i = 0; i < CT_KEYS; i++)
                for (int a = 0; a < CT_COUNT; a++)
                    if (a != c && have[d][a] && has(d, binds[d][c][i], a)) binds[d][c][i] = 0;
        }
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
    fprintf(f, "# bunzi controls (change them on the title screen): an action, then up to %d keys.\n"
               "# pad- lines are the gamepad's buttons; A B X Y are the bottom, right, left and\n"
               "# top face buttons, whatever the pad calls them.\n", CT_KEYS);
    for (int d = 0; d < NDEV; d++)
        for (int c = 0; c < CT_COUNT; c++) {
            fprintf(f, "%s%s", d == DEV_PAD ? "pad-" : "", CT_FILE[c]);
            for (int i = 0; i < CT_KEYS; i++)
                if (binds[d][c][i])
                    fprintf(f, " %s", d == DEV_PAD ? PB_NAME[binds[d][c][i]] : ctl_key_name(binds[d][c][i]));
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
#define TAB_Y     45            /* the keyboard / gamepad tabs, either side of the heading */
#define TAB_X0    58
#define TAB_X1    422
#define NROWS     (CT_COUNT + 2)    /* the tabs (row -1), the actions, DEFAULTS / DONE */
#define HOLD_CANCEL 0.6f        /* holding B this long while waiting for a button cancels */

static const uint32_t INK = 0x10102a, GOLD = 0xffd860, DIM = 0xa8a0d0, WHITE = 0xf4f0e0, PINK = 0xff8aa0,
                      FACE = 0xf2ead6, FACE_HI = 0xfffbef, LIP = 0xb4a487, KEYTXT = 0x2c2450,
                      HOLE = 0x5a5288, OFF = 0x6c6494;

struct CtlScreen {
    Image frame;
    Atlas hero;
    const Sprite *idle[2], *run[6], *jump, *fall, *land, *dash[3], *start, *alert, *ground;
    const Sprite *decor[4];
    const Sprite *puff[3], *dust[4];
    int dev, row, col, bcol;
    bool listening, pad_on;
    float t, sel_t, demo_t, listen_t;
    float b_t0;                 /* when B went down while waiting for a button, or -1 */
    char msg[64];
    uint32_t msg_c;
    float msg_t;
    float flash[CT_COUNT][CT_KEYS];
    bool held[K_MAX], pheld[PB_COUNT];
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
    s->b_t0 = -1;
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

void ctl_screen_open(CtlScreen *s, bool pad)
{
    s->dev = pad ? DEV_PAD : DEV_KEYS;
    s->pad_on = pad;
    s->row = s->col = 0;
    s->bcol = 1;
    s->listening = false;
    s->b_t0 = -1;
    s->t = s->sel_t = s->demo_t = 0;
    s->msg_t = 0;
    memset(s->flash, 0, sizeof s->flash);
    memset(s->held, 0, sizeof s->held);
    memset(s->pheld, 0, sizeof s->pheld);
}

void ctl_screen_select(CtlScreen *s, int row, bool pad, bool listening)
{
    s->dev = pad ? DEV_PAD : DEV_KEYS;
    s->pad_on = true;
    s->row = row < -1 ? -1 : row > CT_COUNT ? CT_COUNT : row;
    s->listening = listening && s->row >= 0 && s->row < CT_COUNT;
}

static void say(CtlScreen *s, uint32_t c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->msg, sizeof s->msg, fmt, ap);
    va_end(ap);
    s->msg_c = c;
    s->msg_t = 2.5f;
}

static void saved(CtlScreen *s)
{
    s->save_failed = ctl_save() != 0;
}

/* a hotkey this key used to be, now that an action has it */
static const char *shortcut(int dev, int key)
{
    if (dev == DEV_PAD) return key == PB_START ? "OVERLAY" : NULL;
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
    int d = s->dev, r = s->row, c = s->col, old = binds[d][r][c];
    int (*tab)[CT_KEYS] = binds[d];
    char kn[16];
    snprintf(kn, sizeof kn, "%s", label(d, key));
    if (old == key) { s->flash[r][c] = 1; snd_play(SFX_SELECT, 1.0f, 1.0f, 0); return; }
    s->msg_t = 0;
    for (int r2 = 0; r2 < CT_COUNT; r2++)
        for (int c2 = 0; c2 < CT_KEYS; c2++) {
            if (tab[r2][c2] != key) continue;
            if (r2 != r && !old && count_keys(d, r2) == 1) {
                say(s, PINK, "%s IS THE ONLY %s %s", kn, CT_LABEL[r2], WORD[d]);
                snd_play(SFX_MENU, 1.0f, 0.7f, 0);
                return;
            }
            tab[r2][c2] = old;      /* a swap, or a move within the row */
            s->flash[r2][c2] = 1;
            if (r2 != r && old) say(s, GOLD, "SWAPPED: %s NOW HAS %s", CT_LABEL[r2], label(d, old));
            else if (r2 != r) say(s, GOLD, "%s TAKEN FROM %s", kn, CT_LABEL[r2]);
        }
    tab[r][c] = key;
    s->flash[r][c] = 1;
    const char *sc = shortcut(d, key);
    if (sc && s->msg_t <= 0) say(s, DIM, "%s IS NO LONGER THE %s %s", kn, sc, WORD[d]);
    snd_play(SFX_SELECT, 1.0f, 1.0f, 0);
    saved(s);
}

static void stop_listening(CtlScreen *s)
{
    s->listening = false;
    s->b_t0 = -1;
    snd_play(SFX_MENU, 1.0f, 0.8f, 0);
}

static void listen(CtlScreen *s, const CtlNav *in, const bool *pressed, const bool *ppressed, const bool *pdown)
{
    if (s->dev == DEV_KEYS) {
        if (in->back) { stop_listening(s); return; }
        for (int k = 0; k < K_MAX && pressed; k++)
            if (pressed[k] && bindable(DEV_KEYS, k)) {
                s->listening = false;
                assign(s, k);
                return;
            }
        return;
    }
    if (in->esc) { stop_listening(s); return; }
    if (s->b_t0 >= 0) {         /* B: a tap gives it to the action, holding it cancels */
        if (pdown && pdown[PB_B]) {
            if (s->t - s->b_t0 >= HOLD_CANCEL) stop_listening(s);
        } else {
            s->b_t0 = -1;
            s->listening = false;
            assign(s, PB_B);
        }
        return;
    }
    for (int b = 1; b < PB_COUNT && ppressed; b++)
        if (ppressed[b]) {
            if (b == PB_B) { s->b_t0 = s->t; return; }
            s->listening = false;
            assign(s, b);
            return;
        }
}

static void flip(CtlScreen *s)
{
    s->dev ^= 1;
    s->msg_t = 0;
    s->sel_t = 0;
    snd_play(SFX_MENU, 1.0f, s->dev ? 1.12f : 0.9f, 0);
}

bool ctl_screen_update(CtlScreen *s, const CtlNav *in, const bool *pressed, const bool *down,
                       const bool *ppressed, const bool *pdown, float dt)
{
    s->t += dt;
    s->sel_t += dt;
    s->demo_t += dt;
    if (s->msg_t > 0) s->msg_t -= dt;
    for (int r = 0; r < CT_COUNT; r++)
        for (int c = 0; c < CT_KEYS; c++)
            if (s->flash[r][c] > 0) s->flash[r][c] = fmaxf(0, s->flash[r][c] - dt * 1.6f);
    if (!in) return false;
    s->pad_on = in->pad_on;
    if (down) memcpy(s->held, down, sizeof s->held);
    if (pdown) memcpy(s->pheld, pdown, sizeof s->pheld);
    else memset(s->pheld, 0, sizeof s->pheld);
    if (s->listening) {
        listen(s, in, pressed, ppressed, pdown);
        return false;
    }
    if (in->back) { snd_play(SFX_MENU, 1.0f, 0.8f, 0); return true; }
    if (in->swap) flip(s);
    if (in->up || in->down) {
        s->row = (s->row + 1 + (in->up ? NROWS - 1 : 1)) % NROWS - 1;
        s->sel_t = s->demo_t = 0;
        snd_play(SFX_MENU, 1.0f, 1.0f, 0);
    }
    int (*tab)[CT_KEYS] = binds[s->dev];
    if (s->row < 0) {
        if (in->left || in->right || in->confirm) flip(s);
    } else if (s->row < CT_COUNT) {
        if (in->left || in->right) {
            s->col = (s->col + (in->left ? CT_KEYS - 1 : 1)) % CT_KEYS;
            s->sel_t = 0;
            snd_play(SFX_MENU, 1.0f, in->right ? 1.08f : 0.94f, 0);
        }
        if (in->confirm) {
            s->listening = true;
            s->listen_t = 0;
            s->b_t0 = -1;
            s->msg_t = 0;
            snd_play(SFX_SELECT, 1.0f, 1.2f, 0);
        } else if (in->clear && tab[s->row][s->col]) {
            if (count_keys(s->dev, s->row) == 1) {
                say(s, PINK, "%s NEEDS A %s", CT_LABEL[s->row], WORD[s->dev]);
                snd_play(SFX_MENU, 1.0f, 0.7f, 0);
            } else {
                tab[s->row][s->col] = 0;
                snd_play(SFX_MENU, 1.0f, 0.85f, 0);
                saved(s);
            }
        }
    } else {
        if (in->left || in->right) { s->bcol ^= 1; snd_play(SFX_MENU, 1.0f, 1.0f, 0); }
        if (in->confirm) {
            if (s->bcol) { snd_play(SFX_SELECT, 1.0f, 1.0f, 0); return true; }
            memcpy(tab, DEFAULTS[s->dev], sizeof binds[0]);
            for (int r = 0; r < CT_COUNT; r++)
                for (int c = 0; c < CT_KEYS; c++) s->flash[r][c] = tab[r][c] ? 1 : 0;
            say(s, GOLD, "%s DEFAULTS RESTORED", s->dev ? "GAMEPAD" : "KEYBOARD");
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

/* the face buttons as a diamond of four with one lit, and the d-pad as a cross
 * with one arm lit: code \1-\11 as in PB_LABEL. 11x11 big, 6x6 / 7x7 small. */
static void pad_icon(Canvas *cv, int x, int y, int code, bool big, uint32_t on, uint32_t off)
{
    if (code <= 4) {
        static const int P[4][2] = { { 1, 2 }, { 2, 1 }, { 0, 1 }, { 1, 0 } };     /* A B X Y */
        int d = big ? 3 : 2, st = big ? 4 : 2;
        for (int i = 0; i < 4; i++)
            gfx_fill(cv, x + P[i][0] * st, y + P[i][1] * st, d, d, i == code - 1 ? on : off);
        return;
    }
    int n = big ? 11 : 7, a = (n - 3) / 2;
    bool all = code == 9;
    gfx_fill(cv, x + a, y, 3, n, all ? on : off);
    gfx_fill(cv, x, y + a, n, 3, all ? on : off);
    switch (code) {
    case 5: gfx_fill(cv, x + a, y, 3, a, on); break;
    case 6: gfx_fill(cv, x + a, y + n - a, 3, a, on); break;
    case 7: gfx_fill(cv, x, y + a, a, 3, on); break;
    case 8: gfx_fill(cv, x + n - a, y + a, a, 3, on); break;
    }
}

/* small text that may hold \1-\11 button pictures */
static int glyph_w(unsigned char ch) { return ch >= 1 && ch <= 4 ? 8 : ch >= 5 && ch <= 9 ? 9 : 4; }

static int rich_width(const char *m)
{
    int w = 0;
    for (; *m; m++) w += glyph_w((unsigned char)*m);
    return w ? w - 1 : 0;
}

static void rich(Canvas *cv, int x, int y, const char *m, uint32_t c)
{
    for (; *m; m++) {
        unsigned char ch = (unsigned char)*m;
        if (ch >= 1 && ch <= 9) {
            int ix = x + 1, iy = y - (ch <= 4 ? 0 : 1);
            pad_icon(cv, ix + 1, iy + 1, ch, false, INK, INK);
            pad_icon(cv, ix, iy, ch, false, c, gfx_mix(c, INK, 120));
        } else if (ch != ' ') {
            char b[2] = { (char)ch, 0 };
            gfx_text(cv, x + 1, y + 1, b, INK);
            gfx_text(cv, x, y, b, c);
        }
        x += glyph_w(ch);
    }
}

static void keycap(Canvas *cv, int x, int y, int dev, int key, bool sel, bool listen, bool held, float flash,
                   float t)
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
    const char *n = label(dev, key);
    unsigned char ic = (unsigned char)n[0];
    if (dev == DEV_KEYS && key >= K_LEFT && key <= K_DOWN)
        arrow(cv, x + (w - 10) / 2, fy + (fh - 10) / 2 + 1, key, tc);
    else if (dev == DEV_PAD && ic >= 1 && ic <= 9)
        pad_icon(cv, x + (w - 11) / 2, fy + (fh - 11) / 2, ic, true, tc, gfx_mix(LIP, KEYTXT, 70));
    else
        gfx_text_scaled(cv, x + (w - gfx_text_width(n, 2)) / 2, fy + (fh - 10) / 2 + 1, 2, n, tc);
    if (flash > 0)                          /* a few sparks fly off a newly set key */
        for (int k = 0; k < 6; k++) {
            uint32_t hh = hash2(k, x * 7 + y);
            float a = (float)(hh & 1023) / 1023.0f * 6.283f, u = 1 - flash;
            int sx = x + w / 2 + (int)(cosf(a) * (w / 2 + 4 + u * 14)), sy = y + h / 2 + (int)(sinf(a) * (8 + u * 10));
            star(cv, sx, sy, flash > 0.5f ? 2 : 1, GOLD);
        }
}

/* the tabs' pictures, drawn at 2x: x the body, o a dark hole */
static void icon2(Canvas *cv, int x, int y, const char *const *rows, int nr, uint32_t c)
{
    for (int j = 0; j < nr; j++)
        for (int i = 0; rows[j][i]; i++) {
            char ch = rows[j][i];
            if (ch == '.') continue;
            gfx_fill(cv, x + i * 2, y + j * 2, 2, 2, ch == 'x' ? c : INK);
        }
}

static void tabs(const CtlScreen *s, Canvas *cv)
{
    static const char *const KB[5] = {
        "xxxxxxxxxxx", "xoxoxoxoxox", "xxxxxxxxxxx", "xxooooooxxx", "xxxxxxxxxxx",
    };
    static const char *const GP[6] = {
        ".xxxxxxxxx.", "xxoxxxxxoxx", "xoooxxxoxox", "xxoxxxxxoxx", "xxxxxxxxxxx", "xxx.....xxx",
    };
    static const char *const NAME[NDEV] = { "KEYBOARD", "GAMEPAD" };
    for (int d = 0; d < NDEV; d++) {
        bool on = s->dev == d, foc = on && s->row < 0;
        int tw = gfx_text_width(NAME[d], 2), w = 22 + 6 + tw;
        int x = d == DEV_KEYS ? TAB_X0 : TAB_X1 - w;
        uint32_t c = on ? GOLD : OFF;
        if (on) {
            float pu = 0.5f + 0.5f * sinf(s->sel_t * 5);
            gfx_glow(cv, x + w / 2, TAB_Y + 5, w / 2 + 10, 0xffb040, (int)((foc ? 30 : 14) + (foc ? 25 : 0) * pu));
        }
        if (foc) {
            paw(cv, x - 12, TAB_Y + 2, GOLD);
            paw(cv, x + w + 7, TAB_Y + 2, GOLD);
        }
        if (d == DEV_KEYS) { icon2(cv, x + 1, TAB_Y + 1, KB, 5, INK); icon2(cv, x, TAB_Y, KB, 5, c); }
        else { icon2(cv, x + 1, TAB_Y, GP, 6, INK); icon2(cv, x, TAB_Y - 1, GP, 6, c); }
        txt(cv, x + 28, TAB_Y, 2, NAME[d], on ? GOLD : OFF);
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
    tabs(s, cv);

    bool pad = s->dev == DEV_PAD;
    const char *hint;
    if (s->listening)
        hint = pad ? "PRESS THE NEW BUTTON  -  HOLD \2 OR ESC TO CANCEL" : "PRESS THE NEW KEY  -  ESC CANCELS";
    else if (pad && s->pad_on)
        hint = "\11 CHOOSE   \1 CHANGE   \4 CLEAR   \2 BACK   LB RB KEYBOARD";
    else if (pad)
        hint = "NO GAMEPAD FOUND   -   ARROWS CHOOSE   ENTER CHANGE   ESC BACK   TAB KEYBOARD";
    else
        hint = "ARROWS CHOOSE   ENTER CHANGE   BKSP CLEAR   ESC BACK   TAB GAMEPAD";
    rich(cv, (cv->w - rich_width(hint)) / 2, 58, hint, pad && !s->pad_on && !s->listening ? PINK : DIM);

    for (int r = 0; r < CT_COUNT; r++) {
        int y = ROW_Y + r * ROW_DY;
        bool on = r == s->row;
        if (on) gfx_tint(cv, LABEL_X - 6, y - 2, SLOT_X + CT_KEYS * SLOT_DX - LABEL_X, SLOT_H + 4, 0xffd860, 22);
        txt(cv, LABEL_X, y + 3, 2, CT_LABEL[r], on ? GOLD : DIM);
        for (int c = 0; c < CT_KEYS; c++) {
            int k = binds[s->dev][r][c];
            bool sel = on && c == s->col;
            bool held = k && (pad ? s->pheld[k] : s->held[k]);
            keycap(cv, SLOT_X + c * SLOT_DX, y, s->dev, k, sel, sel && s->listening, held, s->flash[r][c], s->sel_t);
        }
    }

    /* below: the stage on the left, the buttons on the right */
    demo(s, cv);
    char b[80];
    const char *m;
    uint32_t mc = DIM;
    int sc = 1;
    if (s->listening) {
        if (s->b_t0 >= 0) snprintf(b, sizeof b, "KEEP HOLDING TO CANCEL");
        else snprintf(b, sizeof b, "PRESS A %s FOR %s", WORD[s->dev], CT_LABEL[s->row]);
        m = b;
        mc = ((int)(s->t * 3) & 1) ? GOLD : WHITE;
        sc = 2;
    } else if (s->msg_t > 0) { m = s->msg; mc = s->msg_c; }
    else if (s->save_failed) { m = "COULD NOT SAVE THE CONTROLS"; mc = PINK; }
    else if (s->row < 0) m = "SET UP THE KEYBOARD OR THE GAMEPAD";
    else if (s->row < CT_COUNT) m = CT_HELP[s->dev][s->row];
    else m = s->bcol ? "BACK TO THE TITLE" : pad ? "PUT EVERY BUTTON BACK AS IT WAS" : "PUT EVERY KEY BACK AS IT WAS";
    if (sc == 1) rich(cv, (STAGE_X0 + STAGE_X1 - rich_width(m)) / 2, 179, m, mc);
    else txt(cv, (STAGE_X0 + STAGE_X1 - gfx_text_width(m, sc)) / 2, 179, sc, m, mc);

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
