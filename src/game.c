#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "level.h"
#include "sound.h"

#define PW 10               /* player hitbox */
#define PH 16
#define MAX_PARTICLES 128
#define START_HEARTS 3
#define MAX_VARIANTS 24

#define WALK_SPEED  150.0f
#define RUN_SPEED   250.0f
#define GRAVITY     1150.0f
#define JUMP_VEL    390.0f
#define JUMP_CUT    130.0f
#define MAX_FALL    460.0f
#define STOMP_VEL   300.0f

typedef enum { TH_MOON, TH_LAKE, TH_ISLAND, TH_CAVE } Theme;

typedef struct {
    const char *dir, *file, *title;
    Theme theme;
    const char *hangs;      /* decor that hangs from the cell above */
} LevelDef;

static const LevelDef LEVELS[] = {
    { "moonlit_ruins", "level1.txt", "MOONLIT RUINS", TH_MOON, "LBv" },
    { "sunset_lake",   "level2.txt", "SUNSET LAKE",   TH_LAKE, "v" },
    { "tropical_island", "level3.txt", "TROPICAL ISLAND", TH_ISLAND, "" },
    { "crystal_caverns", "level4.txt", "CRYSTAL CAVERNS", TH_CAVE, "VHh" },
};
#define NLEVELS ((int)(sizeof LEVELS / sizeof LEVELS[0]))

typedef struct { float x, y; bool taken; } Coin;
typedef enum { EN_GHOST, EN_SHROOM, EN_SNAIL, EN_CRAB, EN_JELLY, EN_BIRD,
               EN_BAT, EN_SLIME, EN_WORM, EN_FISH, EN_COUNT } EnemyKind;
typedef struct {
    EnemyKind kind;
    float x, y;             /* fliers: centre; walkers: feet */
    float x0, x1, vx, t, dead_t;
    float y0, y1;           /* jellyfish, fish: vertical range of the centre */
    float vy, die_t;        /* death: fall speed, time since the stomp; fish: leap speed */
    float cool;             /* fish, jellyfish: time until the next leap or squirt */
    int dead_flags;
    bool alive, seen, falling;
} Enemy;

/* animation rows cut from a creature sheet: <kind>_<row><frame> */
enum { AN_IDLE, AN_MOVE, AN_ATTACK, AN_HIT, AN_SPLAT, AN_ROWS };
typedef struct { const Sprite *f[AN_ROWS][3]; int n[AN_ROWS]; } Anim;
static const char *const AN_ROW[AN_ROWS] = { "idle", "move", "attack", "hit", "splat" };
static const char *const AN_KIND[EN_COUNT] = {
    [EN_SHROOM] = "shroom", [EN_SNAIL] = "snail", [EN_CRAB] = "crab", [EN_JELLY] = "jelly",
    [EN_BAT] = "bat", [EN_SLIME] = "slime", [EN_WORM] = "worm", [EN_FISH] = "fish",
};
/* which way the sheet's art faces: 1 right, -1 left, 0 straight at us (never flipped) */
static const signed char AN_FACE[EN_COUNT] = { [EN_SNAIL] = -1, [EN_SLIME] = 1, [EN_FISH] = -1, [EN_WORM] = 1 };
#define FISH_G      900.0f  /* a fish's leap: gravity, highest reach above the surface */
#define FISH_REACH  9
#define SHOT_G      420.0f  /* a jellyfish's water squirt */
#define MAX_SHOTS   16
#define MAX_POPS    8
/* seconds off the clock for each kind of enemy defeated (balance here) */
static const float KILL_BONUS[] = {
    [EN_GHOST] = 0.5f, [EN_SHROOM] = 0.5f, [EN_SNAIL] = 0.5f, [EN_CRAB] = 0.5f, [EN_JELLY] = 0.5f,
    [EN_BIRD] = 0.5f, [EN_BAT] = 0.5f, [EN_SLIME] = 0.5f, [EN_WORM] = 0.5f, [EN_FISH] = 0.5f,
};
#define WORM_PERIOD 4.2f
typedef struct {
    float x, y, vx, vy, t, life;
    const Sprite *fr[4];
    int nf, flags;
} Particle;
typedef struct { float x, y, vx, vy, t; bool live; } Shot;
typedef struct { float x, y, t; char txt[8]; } Pop;     /* a rising "-0.5S" */
#define MAX_MOTES 48
typedef struct { float x, y, vx, vy, t, life, ph; uint32_t c; } Mote;  /* the exit's drifting lights */
typedef struct { int x, y; const Sprite *s; int flags; bool back; int glow_dx, glow_dy; uint32_t glow_c; } Deco;
typedef struct { int tx, ty, len; const Sprite *s; } PlatRun;
typedef enum { SP_BRIDGE, SP_RAIL, SP_ROPE } SpanKind;
typedef struct { SpanKind kind; int tx, ty, len; } Span;

typedef enum { ST_PLAY, ST_DEAD, ST_CLEAR } State;

typedef struct {
    float x, y, vx, vy;
    bool ground, on_plat;
    float coyote, jbuf, drop_t;
    int face;
    float anim, idle_t, land_t, hurt_t, inv_t, dust_t;
    int step_frame;
    bool skidding;
    float safe_x, safe_y;
} Player;

/* everything that belongs to one level and is swapped when the level changes */
typedef struct {
    const LevelDef *def;
    Atlas atlas;
    Image far, mid[2];
    Level lv;
} Stage;

typedef struct { const Sprite *s[MAX_VARIANTS]; int n; } Variants;

struct Game {
    char asset_dir[1024];
    int level;
    Stage st;
    Atlas common;           /* bunny, coins, effects: shared by every level */
    Atlas exits;            /* the burrow door (assets/exit) */
    Atlas fx;               /* water squirts (assets/effects); may be missing */

    /* common sprites */
    const Sprite *coin[4], *sparkle, *dust[3], *puff[2], *splash, *goal;
    const Sprite *squirt[3][5], *wsplash[2], *wspray[2];   /* NULL without assets/effects */
    const Sprite *b_idle[2], *b_run[6], *b_jump, *b_fall[2], *b_land, *b_skid, *b_hurt;
    /* theme sprites (NULL when the theme lacks them) */
    const Sprite *top[6], *stone[6], *spike, *ghost[5], *ghostpoof;
    const Sprite *shroom[3], *shroomflat, *snail[2];
    const Sprite *capl, *capr, *bpost, *planks, *fpost, *rail, *rpost;
    const Sprite *botl, *botr, *pl, *pr, *crab[4], *jelly[4], *bird[4];
    const Sprite *dpost, *dshaft, *dsplash, *dbeam, *dend;
    uint32_t sand, rockedge, rockrim;
    const Sprite *rocktex;
    Anim anim[EN_COUNT];
    int water_y;            /* world y of the highest water surface */
    Variants cap, cob, fg, fgblur, flower, grass, bush, vine, rock, toadstool, fencebit;
    Variants sidel, sider, bot, dot, pm, palm, bigrock, crate, barrel, moor, log;

    Deco *deco;   int ndeco;
    PlatRun *plat; int nplat;
    Span *span;   int nspan;
    Coin *coins;  int ncoins;
    Enemy *enemies; int nenemies;
    int goal_tx, goal_ty;
    bool has_goal;

    Particle part[MAX_PARTICLES];
    Shot shot[MAX_SHOTS];
    Pop pop[MAX_POPS];
    Mote mote[MAX_MOTES];
    float mote_t;
    Player p;
    float camx, camy;
    State state;
    float state_t, time, level_time;
    int hearts, coins_got, coin_combo;
    float coin_t;
    bool clock_on;          /* the clock starts with the first move */
    bool practice;          /* a debug teleport was used: the run is not recorded */
    int kills;
    float bonus, bonus_t;   /* seconds earned from kills; flash on the clock */
    uint64_t level_hash;    /* the layout's fingerprint, for the score tables */
    uint32_t rng;
};

/* ---------------------------------------------------------------- utils */

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float frand(Game *g)
{
    g->rng ^= g->rng << 13; g->rng ^= g->rng >> 17; g->rng ^= g->rng << 5;
    return (g->rng & 0xffffff) / (float)0x1000000;
}

/* stereo position of a world x coordinate */
static float pan_at(const Game *g, float x)
{
    return ((x - g->camx) / VIEW_W * 2.0f - 1.0f) * 0.6f;
}

static void sfx(Game *g, Sfx id, float vol, float x)
{
    snd_play(id, vol, 0.96f + 0.08f * frand(g), pan_at(g, x));
}

static bool overlap(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void add_particle(Game *g, float x, float y, float vx, float vy, float life,
                         const Sprite *const *fr, int nf, int flags)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *q = &g->part[i];
        if (q->life > 0) continue;
        q->x = x; q->y = y; q->vx = vx; q->vy = vy;
        q->t = 0; q->life = life; q->nf = nf; q->flags = flags;
        for (int k = 0; k < nf && k < 4; k++) q->fr[k] = fr[k];
        return;
    }
}

static const Sprite *pick(const Variants *v, uint32_t h)
{
    return v->n ? v->s[h % (uint32_t)v->n] : NULL;
}

static bool standable(int t) { return t == T_SOLID || t == T_PLAT; }

/* ------------------------------------------------------------ level list */

int game_level_count(void) { return NLEVELS; }

const char *game_level_path(int level, const char *asset_dir, char *buf, size_t n)
{
    if (level < 0 || level >= NLEVELS) return NULL;
    snprintf(buf, n, "%s/%s/%s", asset_dir, LEVELS[level].dir, LEVELS[level].file);
    return buf;
}

const char *game_level_title(int level)
{
    return level >= 0 && level < NLEVELS ? LEVELS[level].title : "";
}

const char *game_level_hangs(int level)
{
    return level >= 0 && level < NLEVELS ? LEVELS[level].hangs : "";
}

/* -------------------------------------------------------------- loading */

static const Sprite *need(const Atlas *a, const char *name, char *err, size_t errn)
{
    const Sprite *s = atlas_find(a, name);
    if (!s && !err[0]) snprintf(err, errn, "sprite '%s' missing from atlas", name);
    return s;
}

static void variants(Variants *v, const Atlas *a, const char *prefix)
{
    char nm[24];
    for (v->n = 0; v->n < MAX_VARIANTS; v->n++) {
        snprintf(nm, sizeof nm, "%s%d", prefix, v->n);
        if (!(v->s[v->n] = atlas_find(a, nm))) break;
    }
}

static void stage_free(Stage *s)
{
    atlas_free(&s->atlas);
    image_free(&s->far);
    image_free(&s->mid[0]);
    image_free(&s->mid[1]);
    level_free(&s->lv);
}

static int load_image(Image *im, const char *dir, const char *sub, const char *file, char *err, size_t errn)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s/gen/%s", dir, sub, file);
    if (image_load(im, p)) { snprintf(err, errn, "cannot load %.200s", p); return -1; }
    return 0;
}

static int stage_load(Stage *s, const char *dir, int level, char *err, size_t errn)
{
    memset(s, 0, sizeof *s);
    const LevelDef *d = s->def = &LEVELS[level];
    char a[1400], b[1400];
    snprintf(a, sizeof a, "%s/%s/gen/sprites.png", dir, d->dir);
    snprintf(b, sizeof b, "%s/%s/gen/sprites.txt", dir, d->dir);
    if (atlas_load(&s->atlas, a, b)) { snprintf(err, errn, "cannot load %.200s", a); goto fail; }
    if (load_image(&s->far, dir, d->dir, "bg_far.png", err, errn)) goto fail;
    if (d->theme == TH_MOON) {
        if (load_image(&s->mid[0], dir, d->dir, "bg_trees.png", err, errn)) goto fail;
        if (load_image(&s->mid[1], dir, d->dir, "bg_pines.png", err, errn)) goto fail;
    } else if (d->theme == TH_LAKE) {
        if (load_image(&s->mid[0], dir, d->dir, "bg_lake.png", err, errn)) goto fail;
    } else if (d->theme == TH_CAVE) {
        if (load_image(&s->mid[0], dir, d->dir, "bg_mine.png", err, errn)) goto fail;
    } else {
        if (load_image(&s->mid[0], dir, d->dir, "bg_sea.png", err, errn)) goto fail;
        if (load_image(&s->mid[1], dir, d->dir, "bg_clouds.png", err, errn)) goto fail;
    }
    snprintf(a, sizeof a, "%s/%s/%s", dir, d->dir, d->file);
    if (level_load(&s->lv, a, err, errn)) goto fail;
    return 0;
fail:
    stage_free(s);
    return -1;
}

/* look up the theme's sprites; only theme-essential ones are required */
static int bind_theme(Game *g, char *err, size_t errn)
{
    const Atlas *A = &g->st.atlas;
    char nm[24];
    memset(g->top, 0, sizeof g->top);
    memset(g->stone, 0, sizeof g->stone);
    memset(g->ghost, 0, sizeof g->ghost);
    if (g->st.def->theme == TH_MOON) {
        for (int i = 0; i < 6; i++) {
            snprintf(nm, sizeof nm, "top%d", i);   g->top[i] = need(A, nm, err, errn);
            snprintf(nm, sizeof nm, "stone%d", i); g->stone[i] = need(A, nm, err, errn);
        }
        for (int i = 0; i < 5; i++) { snprintf(nm, sizeof nm, "ghost%d", i); g->ghost[i] = need(A, nm, err, errn); }
    }
    g->spike = atlas_find(A, "spikes");
    g->ghostpoof = atlas_find(A, "ghostpoof");
    for (int i = 0; i < 3; i++) { snprintf(nm, sizeof nm, "shroom%d", i); g->shroom[i] = atlas_find(A, nm); }
    g->shroomflat = atlas_find(A, "shroomflat");
    g->snail[0] = atlas_find(A, "snail0");
    g->snail[1] = atlas_find(A, "snail1");
    g->capl = atlas_find(A, "capl");
    g->capr = atlas_find(A, "capr");
    g->bpost = atlas_find(A, "bpost");
    g->planks = atlas_find(A, "planks");
    g->fpost = atlas_find(A, "fpost");
    g->rail = atlas_find(A, "rail");
    g->rpost = atlas_find(A, "rpost");
    variants(&g->cap, A, "cap");
    variants(&g->cob, A, "cob");
    variants(&g->fg, A, "fg");
    variants(&g->fgblur, A, "fgblur");
    variants(&g->flower, A, "flower");
    variants(&g->grass, A, "grass");
    variants(&g->bush, A, "bush");
    variants(&g->vine, A, "vine");
    variants(&g->rock, A, "rock");
    variants(&g->toadstool, A, "toadstool");
    variants(&g->fencebit, A, "fencebit");
    variants(&g->sidel, A, "sidel");
    variants(&g->sider, A, "sider");
    variants(&g->bot, A, "bot");
    variants(&g->dot, A, "dot");
    variants(&g->pm, A, "pm");
    variants(&g->palm, A, "palm");
    variants(&g->bigrock, A, "bigrock");
    variants(&g->crate, A, "crate");
    variants(&g->barrel, A, "barrel");
    variants(&g->moor, A, "moor");
    variants(&g->log, A, "log");
    g->dpost = atlas_find(A, "dpost");
    g->dshaft = atlas_find(A, "dshaft");
    g->dsplash = atlas_find(A, "dsplash");
    g->dbeam = atlas_find(A, "dbeam");
    g->dend = atlas_find(A, "dend");
    g->botl = atlas_find(A, "botl");
    g->botr = atlas_find(A, "botr");
    g->pl = atlas_find(A, "pl");
    g->pr = atlas_find(A, "pr");
    for (int i = 0; i < 4; i++) {
        snprintf(nm, sizeof nm, "crab%d", i);  g->crab[i] = atlas_find(A, nm);
        snprintf(nm, sizeof nm, "jelly%d", i); g->jelly[i] = atlas_find(A, nm);
        snprintf(nm, sizeof nm, "bird%d", i);  g->bird[i] = atlas_find(A, nm);
    }
    const Sprite *sf = atlas_find(A, "sandfill");     /* a 1x1 swatch of the sand colour */
    g->sand = sf ? A->img.px[sf->y * A->img.w + sf->x] & 0xffffff : 0xfcd083;
    sf = atlas_find(A, "rockedge");
    g->rockedge = sf ? A->img.px[sf->y * A->img.w + sf->x] & 0xffffff : 0x161321;
    sf = atlas_find(A, "rockrim");
    g->rockrim = sf ? A->img.px[sf->y * A->img.w + sf->x] & 0xffffff : 0x292236;
    g->rocktex = atlas_find(A, "rocktex");
    for (int k = 0; k < EN_COUNT; k++)
        for (int r = 0; r < AN_ROWS; r++) {
            Anim *an = &g->anim[k];
            an->n[r] = 0;
            if (!AN_KIND[k]) continue;
            for (int i = 0; i < 3; i++) {
                snprintf(nm, sizeof nm, "%s_%s%d", AN_KIND[k], AN_ROW[r], i);
                if (!(an->f[r][i] = atlas_find(A, nm))) break;
                an->n[r]++;
            }
        }
    if (g->st.def->theme == TH_CAVE && !err[0] &&
        (!g->rocktex || !g->cap.n || !g->capl || !g->capr || !g->anim[EN_BAT].n[AN_MOVE] ||
         !g->anim[EN_SLIME].n[AN_MOVE] || !g->anim[EN_WORM].n[AN_IDLE] || g->anim[EN_WORM].n[AN_IDLE] < 3 ||
         !g->anim[EN_FISH].n[AN_MOVE] || !g->anim[EN_SHROOM].n[AN_MOVE] || !g->anim[EN_SNAIL].n[AN_MOVE]))
        snprintf(err, errn, "%s: crystal caverns sprites missing", g->st.def->dir);
    if (g->st.def->theme == TH_ISLAND && !err[0] &&
        (!g->cap.n || !g->capl || !g->capr || !g->sidel.n || !g->sider.n || !g->bot.n || !g->botl || !g->botr ||
         !g->pl || !g->pr || !g->pm.n || !g->crab[3] || !g->jelly[3] || !g->bird[3] ||
         !g->dpost || !g->dshaft || !g->dbeam || !g->dend))
        snprintf(err, errn, "%s: tropical island sprites missing", g->st.def->dir);
    if (g->st.def->theme == TH_LAKE && !err[0] && (!g->cap.n || !g->cob.n || !g->shroom[0] || !g->snail[0]))
        snprintf(err, errn, "%s: sunset lake sprites missing", g->st.def->dir);
    return err[0] ? -1 : 0;
}

/* a light source sprite may carry "<name>.glow" (zero size, anchor = light offset) */
static void deco_glow(const Atlas *A, Deco *d, const char *name)
{
    char nm[32];
    snprintf(nm, sizeof nm, "%s.glow", name);
    const Sprite *gs = atlas_find(A, nm);
    if (gs) { d->glow_dx = gs->ax; d->glow_dy = gs->ay ? gs->ay : 1; }
}

/* one of <prefix>0, <prefix>1, ... chosen by h */
static const Sprite *pick_named(const Atlas *A, const char *prefix, uint32_t h)
{
    char nm[24];
    int n = 0;
    for (;; n++) {
        snprintf(nm, sizeof nm, "%s%d", prefix, n);
        if (!atlas_find(A, nm)) break;
    }
    if (!n) return NULL;
    snprintf(nm, sizeof nm, "%s%u", prefix, h % (uint32_t)n);
    return atlas_find(A, nm);
}

/* cave decor: a sprite from the first prefix list entry h picks, lit if it has a .glow */
static void cave_deco(const Atlas *A, Deco *d, const char *const *prefixes, int np, uint32_t h)
{
    const char *pre = prefixes[h % (uint32_t)np];
    d->s = pick_named(A, pre, h >> 4);
    if (!d->s) d->s = atlas_find(A, pre);
    if (!d->s) return;
    deco_glow(A, d, d->s->name);
    if (!strncmp(pre, "crystal", 7) || !strncmp(pre, "crock", 5)) d->glow_c = 0x2a70ff;
    else if (!strncmp(pre, "amethyst", 8)) d->glow_c = 0x8040ff;
}

static void add_run(Game *g, int tx, int ty, char c, SpanKind kind)
{
    const Level *lv = &g->st.lv;
    if (level_ch(lv, tx - 1, ty) == c) return;
    int len = 0;
    while (level_ch(lv, tx + len, ty) == c) len++;
    g->span[g->nspan++] = (Span){ kind, tx, ty, len };
}

static void build_world(Game *g)
{
    const Level *lv = &g->st.lv;
    const Atlas *A = &g->st.atlas;
    Theme th = g->st.def->theme;
    bool moon = th == TH_MOON;
    int cells = lv->w * lv->h;
    free(g->deco); free(g->plat); free(g->span); free(g->coins); free(g->enemies);
    g->deco = calloc((size_t)cells, sizeof *g->deco);
    g->plat = calloc((size_t)cells, sizeof *g->plat);
    g->span = calloc((size_t)cells, sizeof *g->span);
    g->coins = calloc((size_t)cells, sizeof *g->coins);
    g->enemies = calloc((size_t)cells, sizeof *g->enemies);
    g->ndeco = g->nplat = g->nspan = g->ncoins = g->nenemies = 0;
    g->has_goal = false;
    g->water_y = lv->h * TS;
    for (int i = 0; i < cells; i++)
        if (lv->tile[i] == T_WATER && (i / lv->w) * TS < g->water_y) g->water_y = (i / lv->w) * TS;

    for (int ty = 0; ty < lv->h; ty++)
        for (int tx = 0; tx < lv->w; tx++) {
            char c = level_ch(lv, tx, ty);
            uint32_t h = hash2(tx, ty);
            int bx = tx * TS + TS / 2, by = ty * TS + TS, top = ty * TS;
            Deco d = { bx, by, NULL, (h >> 9) & 1 ? SPR_FLIP : 0, false, 0, 0, 0 };
            char nm[24];
            switch (c) {
            case 'o':
                g->coins[g->ncoins++] = (Coin){ (float)bx, (float)(top + TS / 2), false };
                break;
            case 'g': case 'e': case 'n': case 'c': case 'j': case 'y':
            case 'a': case 's': case 'z': case 'q': {
                Enemy *en = &g->enemies[g->nenemies++];
                memset(en, 0, sizeof *en);
                en->kind = c == 'g' ? EN_GHOST : c == 'e' ? EN_SHROOM : c == 'n' ? EN_SNAIL
                         : c == 'c' ? EN_CRAB : c == 'j' ? EN_JELLY : c == 'y' ? EN_BIRD
                         : c == 'a' ? EN_BAT : c == 's' ? EN_SLIME : c == 'z' ? EN_WORM : EN_FISH;
                bool flier = en->kind == EN_GHOST || en->kind == EN_JELLY || en->kind == EN_BIRD ||
                             en->kind == EN_BAT || en->kind == EN_FISH;
                en->x = (float)bx;
                en->y = (float)(flier ? top + TS / 2 : by);
                if (en->kind == EN_GHOST || en->kind == EN_BIRD || en->kind == EN_BAT) {
                    int reach = en->kind == EN_BIRD ? 7 : en->kind == EN_BAT ? 6 : 5;
                    int l = tx, r = tx;
                    while (l > tx - reach && level_at(lv, l - 1, ty) == T_EMPTY) l--;
                    while (r < tx + reach && level_at(lv, r + 1, ty) == T_EMPTY) r++;
                    en->x0 = (float)(l * TS + 8); en->x1 = (float)(r * TS + 8);
                    float sp = en->kind == EN_BIRD ? 46.0f : en->kind == EN_BAT ? 40.0f : 28.0f;
                    en->vx = (h & 1) ? sp : -sp;
                } else if (en->kind == EN_JELLY) {
                    /* bobs up out of the water and back: free cells above, water below */
                    int u = ty, dn = ty;
                    while (u > ty - 3 && level_at(lv, tx, u - 1) == T_EMPTY) u--;
                    while (dn < ty + 3 && !standable(level_at(lv, tx, dn + 1))) dn++;
                    en->y0 = (float)(u * TS + 8); en->y1 = (float)(dn * TS + 8);
                    en->cool = 1.0f + (float)(h % 5) * 0.3f;
                } else if (en->kind == EN_FISH) {
                    /* lurks just under the surface, swims about its pool and leaps up to
                     * FISH_REACH free cells above it */
                    int surf = ty;
                    while (level_at(lv, tx, surf - 1) == T_WATER) surf--;
                    int u = surf;
                    while (u > surf - FISH_REACH && (level_at(lv, tx, u - 1) == T_EMPTY ||
                                                     level_at(lv, tx, u - 1) == T_PLAT)) u--;
                    en->y0 = (float)(u * TS + 8); en->y1 = (float)(surf * TS + 14);
                    en->y = en->y1;
                    int l = tx, r = tx;
                    while (level_at(lv, l - 1, surf) == T_WATER) l--;
                    while (level_at(lv, r + 1, surf) == T_WATER) r++;
                    en->x0 = (float)(l * TS + 6); en->x1 = (float)(r * TS + TS - 6);
                    en->cool = (float)(h % 7) * 0.2f;
                } else if (en->kind != EN_WORM) {
                    float sp = en->kind == EN_SHROOM ? 32.0f : en->kind == EN_CRAB ? 26.0f
                             : en->kind == EN_SLIME ? 22.0f : 12.0f;
                    en->vx = (h & 1) ? sp : -sp;
                }
                en->t = (float)(h % 100) / 10.0f;
                en->alive = true;
                break;
            }
            case 'X': g->goal_tx = tx; g->goal_ty = ty; g->has_goal = true; break;
            case 'v':
                d.y = top; d.s = pick(&g->vine, h);
                if (level_at(lv, tx, ty - 1) == T_PLAT) {   /* hang from the middle of the platform */
                    int l = tx, r = tx;
                    while (level_at(lv, l - 1, ty - 1) == T_PLAT) l--;
                    while (level_at(lv, r + 1, ty - 1) == T_PLAT) r++;
                    d.x = (l + r + 1) * TS / 2;
                }
                break;
            case 'f': d.s = pick(&g->flower, h); break;
            case 'w': d.s = pick(&g->grass, h); break;
            case 'u': d.s = pick(&g->bush, h); break;
            case '_': add_run(g, tx, ty, c, SP_BRIDGE); break;
            case '=':
                if (level_ch(lv, tx - 1, ty) != '=') {
                    int len = 0;
                    while (level_ch(lv, tx + len, ty) == '=') len++;
                    /* long runs are built from 3- and 2-wide prefabs */
                    for (int off = 0; off < len; ) {
                        int n = len - off >= 3 && len - off != 4 ? 3 : len - off >= 2 ? 2 : 1;
                        const char *pn;
                        if (th == TH_CAVE) break;   /* rock ledges are drawn with the ground */
                        if (th == TH_ISLAND) {      /* built from strips at draw time */
                            g->plat[g->nplat++] = (PlatRun){ tx, ty, len, NULL };
                            break;
                        }
                        if (moon)
                            pn = n == 3 ? (h & 1 ? "plat3a" : "plat3b")
                               : n == 2 ? (const char *[]){ "plat2a", "plat2b", "plat2c", "plat2d" }[h % 4]
                               : "plat1a";
                        else
                            pn = n == 3 ? ((h >> 3) % 3 ? "plat3a" : "platpost") : "plat2a";
                        g->plat[g->nplat++] = (PlatRun){ tx + off, ty, n, atlas_find(A, pn) };
                        off += n;
                    }
                }
                break;
            default:
                if (moon) {
                    switch (c) {
                    case 'L': d.y = top; d.s = atlas_find(A, "lantern"); d.flags = 0; d.glow_dy = 33; break;
                    case 'B': d.y = top; snprintf(nm, sizeof nm, "banner%u", h % 2); d.s = atlas_find(A, nm); d.flags = 0; break;
                    case 'T': d.s = atlas_find(A, "torch"); d.flags = 0; d.glow_dy = -32; break;
                    case 'P': {
                        static const char *P[] = { "pillar0", "pillar1", "pillar3", "pillar4" };
                        d.s = atlas_find(A, P[h % 4]); d.back = true; break;
                    }
                    case 'A': {
                        static const char *P[] = { "archwall0", "archwall1", "arch0" };
                        d.s = atlas_find(A, P[h % 3]); d.back = true; break;
                    }
                    case 'r': snprintf(nm, sizeof nm, "rubble%u", h % 5); d.s = atlas_find(A, nm); d.back = true; break;
                    }
                } else if (th == TH_CAVE) {
                    static const char *const GANTRY[] = { "gantry" }, *const POST[] = { "post", "gibbet" },
                        *const BEAM[] = { "hbeam", "hbeam", "hbracket" }, *const STAL[] = { "stal" },
                        *const CROCK[] = { "crock" }, *const CRATE[] = { "crate" },
                        *const BARREL[] = { "barrel" }, *const LAMP[] = { "lamp" }, *const BONES[] = { "bones", "pebble" },
                        *const TOOL[] = { "pick", "shovel" }, *const MITE[] = { "mite" };
                    bool hang = c == 'V' || c == 'H' || c == 'h';
                    if (hang) d.y = top;
                    switch (c) {
                    case 'L': cave_deco(A, &d, GANTRY, 1, h >> 2); d.back = true; break;
                    case 'P': cave_deco(A, &d, POST, 2, h >> 2); d.back = true; break;
                    case 'H': cave_deco(A, &d, BEAM, 3, h >> 2); d.back = true; break;
                    case 'V': cave_deco(A, &d, STAL, 1, h >> 2); break;
                    case 'h': d.s = atlas_find(A, "chain"); d.back = true; break;
                    case 'K': {     /* the big ones: crystal0-2, amethyst0-2 */
                        static const char *const K[] = { "crystal0", "crystal1", "crystal2",
                                                         "amethyst0", "amethyst1", "amethyst2" };
                        const char *nmk = K[(h >> 3) % 6];
                        d.s = atlas_find(A, nmk);
                        if (d.s) deco_glow(A, &d, nmk);
                        d.glow_c = nmk[0] == 'a' ? 0x8040ff : 0x2a70ff;
                        break;
                    }
                    case 'i': {
                        static const char *const I[] = { "crystal3", "crystal4", "crystal5", "crystal6", "crystal7" };
                        const char *nmk = I[(h >> 3) % 5];
                        d.s = atlas_find(A, nmk);
                        if (d.s) deco_glow(A, &d, nmk);
                        d.glow_c = 0x2a70ff;
                        break;
                    }
                    case 'k': cave_deco(A, &d, CROCK, 1, h >> 2); break;
                    case 'C': cave_deco(A, &d, CRATE, 1, h >> 2); break;
                    case 'D': cave_deco(A, &d, BARREL, 1, h >> 2); break;
                    case 'M': d.s = atlas_find(A, "cart"); break;
                    case 'l': cave_deco(A, &d, LAMP, 1, h >> 2); break;
                    case 'x': cave_deco(A, &d, BONES, 2, h >> 2); break;
                    case 't': d.s = atlas_find(A, TOOL[(h >> 3) & 1]); break;
                    case 'T': cave_deco(A, &d, MITE, 1, h >> 2); d.back = true; break;
                    }
                } else if (th == TH_ISLAND) {
                    switch (c) {
                    case 'P': d.s = pick(&g->palm, h >> 2); d.back = true; break;
                    case 'k': d.s = pick(&g->rock, h); break;
                    case 'K': d.s = pick(&g->bigrock, h); break;
                    case 'C': d.s = pick(&g->crate, h >> 4); break;
                    case 'D': d.s = pick(&g->barrel, h >> 4); d.flags = 0; break;
                    case 'S': d.s = atlas_find(A, "sign"); d.flags = 0; d.back = true; break;
                    case 'b': d.s = pick(&g->moor, h >> 4); break;
                    case 'l': d.s = pick(&g->log, h >> 4); break;
                    }
                } else {
                    switch (c) {
                    case 'L': d.s = atlas_find(A, "lamppost"); d.flags = 0; deco_glow(A, &d, "lamppost"); d.back = true; break;
                    case 'l': d.s = atlas_find(A, "lamppost1"); d.flags = 0; deco_glow(A, &d, "lamppost1"); d.back = true; break;
                    case 'S': d.s = atlas_find(A, "sign"); d.flags = 0; d.back = true; break;
                    case 'k': d.s = pick(&g->rock, h); break;
                    case 'K': d.s = atlas_find(A, "rockbush"); break;
                    case 'M': d.s = pick(&g->toadstool, h); break;
                    case 'b': d.s = pick(&g->fencebit, h); d.back = true; break;
                    case 'F': add_run(g, tx, ty, c, SP_RAIL); break;
                    case 'H': add_run(g, tx, ty, c, SP_ROPE); break;
                    }
                }
            }
            if (d.s) g->deco[g->ndeco++] = d;
        }
}

static void spawn_player(Game *g, float x, float y)
{
    Player *p = &g->p;
    memset(p, 0, sizeof *p);
    p->x = x; p->y = y;
    p->face = 1;
    p->safe_x = x; p->safe_y = y;
}

void game_restart(Game *g)
{
    build_world(g);
    memset(g->part, 0, sizeof g->part);
    float sx = (float)(g->st.lv.start_tx * TS + (TS - PW) / 2);
    float sy = (float)(g->st.lv.start_ty * TS + TS - PH);
    spawn_player(g, sx, sy);
    memset(g->shot, 0, sizeof g->shot);
    memset(g->pop, 0, sizeof g->pop);
    memset(g->mote, 0, sizeof g->mote);
    g->hearts = START_HEARTS;
    g->coins_got = 0;
    g->state = ST_PLAY;
    g->state_t = 0;
    g->level_time = 0;
    g->clock_on = g->practice = false;
    g->kills = 0;
    g->bonus = g->bonus_t = 0;
    g->camx = g->p.x - VIEW_W / 2.0f;
    g->camy = g->p.y - VIEW_H * 0.55f;
}

int game_set_level(Game *g, int level, char *err, size_t errn)
{
    err[0] = 0;
    if (level < 0 || level >= NLEVELS) { snprintf(err, errn, "no level %d", level + 1); return -1; }
    Stage ns;
    if (stage_load(&ns, g->asset_dir, level, err, errn)) return -1;
    Stage old = g->st;
    g->st = ns;
    /* sprites point into the atlas image, so re-point after the move */
    for (int i = 0; i < g->st.atlas.n; i++) g->st.atlas.spr[i].img = &g->st.atlas.img;
    if (bind_theme(g, err, errn)) {
        stage_free(&g->st);
        g->st = old;
        for (int i = 0; i < g->st.atlas.n; i++) g->st.atlas.spr[i].img = &g->st.atlas.img;
        if (old.def) bind_theme(g, (char[8]){ 0 }, 8);
        return -1;
    }
    stage_free(&old);
    g->level = level;
    uint64_t h = 0xcbf29ce484222325ull;     /* FNV-1a over the level's characters */
    const Level *lv = &g->st.lv;
    for (int i = 0; i < lv->w * lv->h; i++) h = (h ^ (uint8_t)lv->ch[i]) * 0x100000001b3ull;
    g->level_hash = (h ^ (uint64_t)lv->w << 32 ^ (uint64_t)lv->h) * 0x100000001b3ull;
    game_restart(g);
    return 0;
}

Game *game_create(const char *asset_dir, int level, char *err, size_t errn)
{
    err[0] = 0;
    Game *g = calloc(1, sizeof *g);
    if (!g) { snprintf(err, errn, "out of memory"); return NULL; }
    g->rng = 0x9E3779B9u;
    snprintf(g->asset_dir, sizeof g->asset_dir, "%s", asset_dir);

    char a[1400], b[1400];
    snprintf(a, sizeof a, "%s/moonlit_ruins/gen/sprites.png", asset_dir);
    snprintf(b, sizeof b, "%s/moonlit_ruins/gen/sprites.txt", asset_dir);
    if (atlas_load(&g->common, a, b)) { snprintf(err, errn, "cannot load %.200s", a); goto fail; }
    for (int i = 0; i < g->common.n; i++) g->common.spr[i].img = &g->common.img;

    const Atlas *C = &g->common;
    char nm[24];
    for (int i = 0; i < 6; i++) { snprintf(nm, sizeof nm, "b_run%d", i); g->b_run[i] = need(C, nm, err, errn); }
    for (int i = 0; i < 4; i++) { snprintf(nm, sizeof nm, "coin%d", i); g->coin[i] = need(C, nm, err, errn); }
    for (int i = 0; i < 3; i++) { snprintf(nm, sizeof nm, "dust%d", i); g->dust[i] = need(C, nm, err, errn); }
    g->puff[0] = need(C, "puff0", err, errn);
    g->puff[1] = need(C, "puff1", err, errn);
    g->splash = need(C, "splash", err, errn);
    g->sparkle = need(C, "sparkle", err, errn);
    snprintf(a, sizeof a, "%s/exit/gen/sprites.png", asset_dir);
    snprintf(b, sizeof b, "%s/exit/gen/sprites.txt", asset_dir);
    if (atlas_load(&g->exits, a, b) == 0) {
        for (int i = 0; i < g->exits.n; i++) g->exits.spr[i].img = &g->exits.img;
        g->goal = atlas_find(&g->exits, "exit_door");
    }
    if (!g->goal) g->goal = need(C, "carve1", err, errn);    /* the old carved stone */
    snprintf(a, sizeof a, "%s/effects/gen/sprites.png", asset_dir);
    snprintf(b, sizeof b, "%s/effects/gen/sprites.txt", asset_dir);
    if (atlas_load(&g->fx, a, b) == 0) {
        for (int i = 0; i < g->fx.n; i++) g->fx.spr[i].img = &g->fx.img;
        for (int f = 0; f < 3; f++)
            for (int k = 0; k < 5; k++) {
                snprintf(nm, sizeof nm, "squirt%d_%d", f, k);
                g->squirt[f][k] = atlas_find(&g->fx, nm);
            }
        for (int f = 0; f < 2; f++) {
            snprintf(nm, sizeof nm, "wsplash%d", f); g->wsplash[f] = atlas_find(&g->fx, nm);
            snprintf(nm, sizeof nm, "wspray%d", f);  g->wspray[f] = atlas_find(&g->fx, nm);
        }
        if (!g->wsplash[1]) g->wsplash[0] = NULL;
        if (!g->wspray[1]) g->wspray[0] = NULL;
    }
    g->b_idle[0] = need(C, "b_idle0", err, errn);
    g->b_idle[1] = need(C, "b_idle1", err, errn);
    g->b_jump = need(C, "b_jump", err, errn);
    g->b_fall[0] = need(C, "b_fall0", err, errn);
    g->b_fall[1] = need(C, "b_fall1", err, errn);
    g->b_land = need(C, "b_land", err, errn);
    g->b_skid = need(C, "b_skid", err, errn);
    g->b_hurt = need(C, "b_hurt", err, errn);
    if (err[0]) goto fail;

    if (game_set_level(g, level, err, errn)) goto fail;
    return g;
fail:
    game_destroy(g);
    return NULL;
}

void game_destroy(Game *g)
{
    if (!g) return;
    atlas_free(&g->common);
    atlas_free(&g->exits);
    atlas_free(&g->fx);
    stage_free(&g->st);
    free(g->deco); free(g->plat); free(g->span); free(g->coins); free(g->enemies);
    free(g);
}

/* ----------------------------------------------------------- simulation */

static void hurt(Game *g, float from_x)
{
    Player *p = &g->p;
    if (p->inv_t > 0 || g->state != ST_PLAY) return;
    g->hearts--;
    p->hurt_t = 0.45f;
    p->inv_t = 1.5f;
    p->vx = (p->x + PW / 2.0f < from_x ? -1.0f : 1.0f) * 170.0f;
    p->vy = -240.0f;
    p->ground = false;
    if (g->hearts <= 0) { g->state = ST_DEAD; g->state_t = 0; }
    sfx(g, g->hearts <= 0 ? SFX_DIE : SFX_HURT, 1.0f, p->x);
}

static void land_dust(Game *g, float strength)
{
    Player *p = &g->p;
    float fx = p->x + PW / 2.0f, fy = p->y + PH;
    int n = strength > 0.6f ? 2 : 1;
    for (int i = 0; i < n; i++) {
        float dir = i ? 1.0f : -1.0f;
        add_particle(g, fx + dir * 5, fy - 3, dir * 25.0f, -8.0f, 0.3f, g->puff, 2, dir > 0 ? 0 : SPR_FLIP);
    }
}

static void move_player(Game *g, float dt)
{
    Player *p = &g->p;
    const Level *lv = &g->st.lv;

    p->x += p->vx * dt;
    int ty0 = (int)floorf(p->y / TS), ty1 = (int)floorf((p->y + PH - 0.001f) / TS);
    if (p->vx > 0) {
        int tx = (int)floorf((p->x + PW - 0.001f) / TS);
        for (int ty = ty0; ty <= ty1; ty++)
            if (level_at(lv, tx, ty) == T_SOLID) { p->x = (float)(tx * TS - PW); p->vx = 0; break; }
    } else if (p->vx < 0) {
        int tx = (int)floorf(p->x / TS);
        for (int ty = ty0; ty <= ty1; ty++)
            if (level_at(lv, tx, ty) == T_SOLID) { p->x = (float)((tx + 1) * TS); p->vx = 0; break; }
    }

    float prev_bottom = p->y + PH;
    bool was_ground = p->ground;
    float impact = p->vy;
    p->ground = p->on_plat = false;
    p->y += p->vy * dt;
    int tx0 = (int)floorf(p->x / TS), tx1 = (int)floorf((p->x + PW - 0.001f) / TS);
    if (p->vy > 0) {
        int ty = (int)floorf((p->y + PH - 0.001f) / TS);
        for (int tx = tx0; tx <= tx1; tx++) {
            int t = level_at(lv, tx, ty);
            bool plat = t == T_PLAT && p->drop_t <= 0 && prev_bottom <= ty * TS + 0.5f;
            if (t == T_SOLID || plat) {
                p->y = (float)(ty * TS - PH);
                p->vy = 0;
                p->ground = true;
                p->on_plat = plat;
                break;
            }
        }
    } else if (p->vy < 0) {
        int ty = (int)floorf(p->y / TS);
        for (int tx = tx0; tx <= tx1; tx++)
            if (level_at(lv, tx, ty) == T_SOLID) { p->y = (float)((ty + 1) * TS); p->vy = 0; break; }
    }
    if (p->ground && !was_ground && impact > 120.0f)
        sfx(g, SFX_LAND, fminf(1.0f, impact / MAX_FALL + 0.2f), p->x);
    if (p->ground && !was_ground && impact > 220.0f) {
        p->land_t = 0.1f;
        land_dust(g, impact / MAX_FALL);
    }
}

/* lose a heart and go back to the last safe footing (pits and water) */
static void fall_out(Game *g)
{
    Player *p = &g->p;
    g->hearts--;
    if (g->hearts <= 0) {
        g->state = ST_DEAD;
        g->state_t = 0;
        sfx(g, SFX_DIE, 1.0f, p->x);
    } else {
        float sx = p->safe_x, sy = p->safe_y;
        spawn_player(g, sx, sy);
        p->inv_t = 1.5f;
        sfx(g, SFX_RESPAWN, 1.0f, p->x);
    }
}

static void update_player(Game *g, const Actions *a, float dt)
{
    Player *p = &g->p;
    const Level *lv = &g->st.lv;
    bool ctl = p->hurt_t <= 0;
    p->hurt_t -= dt;
    p->inv_t -= dt;
    p->land_t -= dt;
    p->drop_t -= dt;

    int dir = ctl ? (a->right ? 1 : 0) - (a->left ? 1 : 0) : 0;
    float target = dir * (a->run ? RUN_SPEED : WALK_SPEED);
    float rate = !ctl ? 300.0f
               : dir == 0 ? (p->ground ? 2200.0f : 500.0f)
               : p->vx * dir < 0 ? (p->ground ? 3200.0f : 1600.0f)
               : p->ground ? 1700.0f : 1200.0f;
    if (p->vx < target) { p->vx += rate * dt; if (p->vx > target) p->vx = target; }
    else if (p->vx > target) { p->vx -= rate * dt; if (p->vx < target) p->vx = target; }
    if (dir) p->face = dir;

    p->jbuf = ctl && a->jump_pressed ? 0.12f : p->jbuf - dt;
    p->coyote = p->ground ? 0.09f : p->coyote - dt;
    if (p->jbuf > 0 && p->on_plat && a->down) {     /* drop through a one-way platform */
        p->drop_t = 0.2f;
        p->jbuf = 0;
        p->ground = false;
        p->y += 1;
    } else if (p->jbuf > 0 && p->coyote > 0) {
        p->vy = -JUMP_VEL - fabsf(p->vx) * 0.12f;   /* running jumps go a little higher */
        p->jbuf = p->coyote = 0;
        p->ground = false;
        add_particle(g, p->x + PW / 2.0f, p->y + PH - 4, 0, -10, 0.25f, g->dust, 3, 0);
        sfx(g, SFX_JUMP, 1.0f, p->x);
    }
    if (ctl && !a->jump && p->vy < -JUMP_CUT) p->vy = -JUMP_CUT;
    p->vy += GRAVITY * dt;
    if (p->vy > MAX_FALL) p->vy = MAX_FALL;

    move_player(g, dt);
    p->anim += fabsf(p->vx) * dt;
    p->idle_t = p->ground && fabsf(p->vx) < 5 ? p->idle_t + dt : 0;

    /* running / skidding dust */
    p->dust_t -= dt;
    bool skid = p->ground && dir && p->vx * dir < -60.0f;
    if (skid && !p->skidding) sfx(g, SFX_SKID, 1.0f, p->x);
    p->skidding = skid;
    int sf = (int)(p->anim / 9.0f) % 6;
    if (p->ground && fabsf(p->vx) > 40.0f && sf != p->step_frame && (sf == 0 || sf == 3))
        snd_play(SFX_STEP, 0.7f + 0.3f * frand(g), 0.85f + 0.3f * frand(g), pan_at(g, p->x));
    p->step_frame = sf;
    if (p->ground && p->dust_t <= 0 && (skid || fabsf(p->vx) > 200.0f)) {
        float back = p->vx > 0 ? -1.0f : 1.0f;
        add_particle(g, p->x + PW / 2.0f + back * 4, p->y + PH - 3, back * 20.0f, -12.0f, 0.3f,
                     g->dust, 3, 0);
        p->dust_t = skid ? 0.05f : 0.09f;
    }

    /* remember solid footing for respawns after a fall */
    if (p->ground && !p->on_plat && p->hurt_t <= 0) {
        int fy = (int)floorf((p->y + PH + 1) / TS);
        int l = (int)floorf((p->x - 2) / TS), r = (int)floorf((p->x + PW + 2) / TS);
        if (level_at(lv, l, fy) == T_SOLID && level_at(lv, r, fy) == T_SOLID &&
            level_at(lv, l, fy - 1) != T_SPIKE && level_at(lv, r, fy - 1) != T_SPIKE) {
            p->safe_x = p->x;
            p->safe_y = p->y;
        }
    }

    /* spikes: the lower part of the tile */
    int tx0 = (int)floorf(p->x / TS), tx1 = (int)floorf((p->x + PW - 0.001f) / TS);
    int ty0 = (int)floorf(p->y / TS), ty1 = (int)floorf((p->y + PH - 0.001f) / TS);
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++)
            if (level_at(lv, tx, ty) == T_SPIKE &&
                overlap(p->x, p->y, PW, PH, tx * TS + 2.0f, ty * TS + 8.0f, TS - 4.0f, 8.0f))
                hurt(g, tx * TS + TS / 2.0f);

    /* water: a splash once the feet are a little under the surface */
    int wy = (int)floorf((p->y + PH - 6) / TS);
    int wx = (int)floorf((p->x + PW / 2.0f) / TS);
    if (level_at(lv, wx, wy) == T_WATER) {
        float sx = p->x + PW / 2.0f;
        int sy = wy;
        while (level_at(lv, wx, sy - 1) == T_WATER) sy--;
        for (int k = 0; k < 3; k++)
            add_particle(g, sx + (k - 1) * 7, (float)(sy * TS), (k - 1) * 30.0f, -60.0f - 25.0f * (k == 1),
                         0.45f, &g->splash, 1, k == 0 ? SPR_FLIP : 0);
        sfx(g, SFX_POOF, 1.0f, sx);
        fall_out(g);
        return;
    }
    if (p->y > lv->h * TS + 40) fall_out(g);
}

/* where a flier is drawn: ghosts and birds bob, jellyfish rise and sink */
static float enemy_y(const Enemy *en)
{
    switch (en->kind) {
    case EN_GHOST: return en->y + sinf(en->t * 2.6f) * 4.0f;
    case EN_BIRD:  return en->y + sinf(en->t * 3.4f) * 6.0f;
    case EN_JELLY: {
        float u = 0.5f - 0.5f * cosf(en->t * 1.3f);         /* 0 top .. 1 bottom */
        return en->y0 + (en->y1 - en->y0) * u * u;          /* lingers near the top */
    }
    case EN_BAT: return en->y + sinf(en->t * 2.0f) * 9.0f;
    default: return en->y;
    }
}

static bool has_anim(const Game *g, EnemyKind k) { return g->anim[k].n[AN_MOVE] || g->anim[k].n[AN_IDLE]; }

/* 0 1 2 1 0 1 ... */
static int pingpong(float t, float fps, int n)
{
    if (n <= 1) return 0;
    int k = (int)(t * fps) % (2 * n - 2);
    return k < n ? k : 2 * n - 2 - k;
}

static const Sprite *anim_get(const Anim *an, int row, int i)
{
    if (!an->n[row]) row = an->n[AN_MOVE] ? AN_MOVE : AN_IDLE;
    if (i >= an->n[row]) i = an->n[row] - 1;
    return i >= 0 ? an->f[row][i] : NULL;
}

/* the frame a living sheet-animated enemy shows now, where (world) and how */
static const Sprite *enemy_frame(const Game *g, const Enemy *en, int *wx, int *wy, int *flags)
{
    const Anim *an = &g->anim[en->kind];
    const Sprite *s;
    int face = en->vx > 0 ? 1 : en->vx < 0 ? -1 : 0;
    float t = en->t;
    switch (en->kind) {
    case EN_WORM: {     /* peeks, rears up, sways (bites when you are close), sinks back */
        float u = fmodf(t, WORM_PERIOD);
        float px = g->p.x + PW / 2.0f;
        face = px < en->x ? -1 : 1;
        bool near = fabsf(px - en->x) < 56.0f && fabsf(g->p.y + PH - en->y) < 48.0f;
        if (u < 1.2f || u >= 3.8f) s = anim_get(an, AN_IDLE, 2);
        else if (u < 1.35f || u >= 3.65f) s = anim_get(an, AN_IDLE, 1);
        else if (u < 1.5f || u >= 3.5f) s = anim_get(an, AN_IDLE, 0);
        else if (near && an->n[AN_ATTACK]) s = anim_get(an, AN_ATTACK, pingpong(u, 8.0f, an->n[AN_ATTACK]));
        else s = anim_get(an, AN_MOVE, pingpong(u - 1.5f, 5.0f, an->n[AN_MOVE]));
        break;
    }
    case EN_FISH:
        face = g->p.x + PW / 2.0f < en->x ? -1 : 1;
        s = anim_get(an, AN_MOVE, pingpong(t, 9.0f, an->n[AN_MOVE]));
        break;
    case EN_BAT:   s = anim_get(an, AN_MOVE, pingpong(t, 10.0f, an->n[AN_MOVE])); break;
    case EN_SLIME: s = anim_get(an, AN_MOVE, pingpong(t, 6.0f, an->n[AN_MOVE])); break;
    case EN_SNAIL: s = anim_get(an, AN_MOVE, pingpong(t, 3.0f, an->n[AN_MOVE])); break;
    case EN_JELLY: s = anim_get(an, AN_MOVE, pingpong(t, 5.0f, an->n[AN_MOVE])); break;
    default:       s = anim_get(an, AN_MOVE, pingpong(t, 7.0f, an->n[AN_MOVE])); break;
    }
    *wx = (int)en->x;
    *wy = (int)enemy_y(en);
    *flags = AN_FACE[en->kind] && face && face != AN_FACE[en->kind] ? SPR_FLIP : 0;
    return s;
}

static void enemy_box(const Game *g, const Enemy *en, float *x, float *y, float *w, float *h)
{
    if (has_anim(g, en->kind)) {        /* the current frame, a little inset */
        int wx, wy, fl;
        const Sprite *s = enemy_frame(g, en, &wx, &wy, &fl);
        if (s) {
            int left = fl & SPR_FLIP ? wx + s->ax - s->w + 1 : wx - s->ax;
            *x = left + 3.0f; *w = fmaxf(6.0f, s->w - 6.0f);
            *y = wy - s->ay + 3.0f; *h = fmaxf(6.0f, s->h - 4.0f);
            return;
        }
    }
    *x = en->x - 8; *y = en->y - 16; *w = 16; *h = 16;
    switch (en->kind) {
    case EN_GHOST:  *x = en->x - 8; *y = enemy_y(en) - 7; *w = 16; *h = 14; break;
    case EN_SHROOM: *x = en->x - 8; *y = en->y - 16; *w = 16; *h = 16; break;
    case EN_SNAIL:  *x = en->x - 11; *y = en->y - 14; *w = 22; *h = 14; break;
    case EN_CRAB:   *x = en->x - 13; *y = en->y - 17; *w = 26; *h = 17; break;
    case EN_JELLY:  *x = en->x - 10; *y = enemy_y(en) - 9; *w = 21; *h = 17; break;
    case EN_BIRD:   *x = en->x - 10; *y = enemy_y(en) - 8; *w = 20; *h = 15; break;
    default: break;
    }
}

static void update_enemy(Game *g, Enemy *en, float dt)
{
    const Level *lv = &g->st.lv;
    float was = enemy_y(en);
    en->t += dt;
    float px = g->p.x + PW / 2.0f, py = g->p.y + PH / 2.0f;
    if (en->kind == EN_FISH) {
        /* under water it trails the bunny about its pool and leaps on a steady beat
         * (bubbles give it away); rested, it also snaps at her as she jumps over. It
         * needs a while after each leap, so a watchful bunny crosses just after one. */
        bool near = en->seen && fabsf(px - en->x) < 130.0f && py < en->y1;
        bool over = near && !g->p.ground && fabsf(px - en->x) < 64.0f && g->p.vy < 120.0f;
        if (en->vy == 0) {
            en->cool -= dt;
            if (near) {
                float step = 35.0f * dt, d = px - en->x;
                en->x += d > step ? step : d < -step ? -step : d;
                en->x = fminf(fmaxf(en->x, en->x0), en->x1);
            }
            if (en->seen && en->cool < 0.45f && fmodf(en->cool, 0.1f) < dt)
                add_particle(g, en->x + (frand(g) - 0.5f) * 10, en->y1 - 15, (frand(g) - 0.5f) * 20, -35.0f,
                             0.25f, &g->splash, 1, frand(g) < 0.5f ? SPR_FLIP : 0);
            if (en->cool <= 0 || (over && en->cool < 1.3f)) {
                /* a snap: just above her; on the beat: as high as she could jump from there */
                float apex = over ? py - 10.0f : near ? fminf(py - 60.0f, en->y1 - 40.0f) : en->y1 - 44.0f;
                apex = fmaxf(apex, en->y0);
                en->vy = -sqrtf(2.0f * FISH_G * (en->y1 - apex));
                en->cool = near ? 1.6f + frand(g) * 0.5f : 2.2f + frand(g) * 1.0f;
            }
        } else {
            en->vy += FISH_G * dt;
            en->y += en->vy * dt;
            if (en->vy < 0 && near) {       /* and steers toward her on the way up */
                float step = 60.0f * dt, d = px - en->x;
                en->x += d > step ? step : d < -step ? -step : d;
                en->x = fminf(fmaxf(en->x, en->x0), en->x1);
            }
            if (en->vy > 0 && en->y >= en->y1) { en->y = en->y1; en->vy = 0; }
        }
        float now = en->y, surf = en->y1 - 14;
        if ((was > surf) != (now > surf) && en->seen) {
            for (int k = 0; k < 2; k++)
                add_particle(g, en->x + (k ? 5 : -5), surf, (k ? 25.0f : -25.0f), -50.0f, 0.4f,
                             &g->splash, 1, k ? 0 : SPR_FLIP);
            if (now < surf) sfx(g, SFX_SPLASH, 0.5f, en->x);
        }
        return;
    }
    if (en->kind == EN_JELLY) {
        /* near the top of its bob it squirts water at the bunny */
        en->cool -= dt;
        float jy = enemy_y(en);
        if (en->cool <= 0 && en->seen && jy < en->y0 + 6 && fabsf(px - en->x) < 170.0f) {
            for (int i = 0; i < MAX_SHOTS; i++) {
                Shot *sh = &g->shot[i];
                if (sh->live) continue;
                /* aimed where she will be, not where she is */
                float T = fminf(fmaxf(fabsf(px - en->x) / 170.0f, 0.45f), 0.95f);
                float ax = px + g->p.vx * T * 0.8f, ay = py + (g->p.ground ? 0.0f : g->p.vy * T * 0.3f);
                *sh = (Shot){ en->x, jy - 6, (ax - en->x) / T, (ay - (jy - 6)) / T - 0.5f * SHOT_G * T, 0, true };
                sfx(g, SFX_SPLASH, 0.6f, en->x);
                if (g->wspray[0]) add_particle(g, en->x, jy - 8, 0, -10.0f, 0.25f, g->wspray, 2, 0);
                break;
            }
            en->cool = 1.8f + frand(g) * 0.8f;
        }
        return;
    }
    if (en->kind == EN_WORM) return;
    if (en->kind == EN_GHOST || en->kind == EN_BIRD || en->kind == EN_BAT) {
        en->x += en->vx * dt;
        if (en->x < en->x0) { en->x = en->x0; en->vx = fabsf(en->vx); }
        if (en->x > en->x1) { en->x = en->x1; en->vx = -fabsf(en->vx); }
        return;
    }
    /* walkers turn at walls and ledges */
    float half = en->kind == EN_SHROOM ? 7.0f : en->kind == EN_CRAB ? 12.0f : en->kind == EN_SLIME ? 10.0f : 10.0f;
    float nx = en->x + en->vx * dt;
    float front = nx + (en->vx > 0 ? half : -half);
    int tx = (int)floorf(front / TS), ty = (int)floorf((en->y - 1) / TS);
    if (level_at(lv, tx, ty) == T_SOLID || !standable(level_at(lv, tx, ty + 1)))
        en->vx = -en->vx;
    else
        en->x = nx;
}

/* a stomped sheet-animated enemy: fliers drop to the ground first, then it splats */
static void update_dead(Game *g, Enemy *en, float dt)
{
    en->die_t += dt;
    if (!en->falling) { en->dead_t -= dt; return; }
    const Level *lv = &g->st.lv;
    en->vy = fminf(en->vy + GRAVITY * 0.7f * dt, MAX_FALL);
    float ny = en->y + en->vy * dt;
    int tx = (int)floorf(en->x / TS), ty = (int)floorf((ny + 6) / TS);
    int t = level_at(lv, tx, ty);
    if (en->vy > 0 && standable(t) && en->y + 6 <= ty * TS + 1) {
        en->y = (float)(ty * TS);
        en->falling = false;
        en->die_t = 0.18f;             /* straight to the splat */
        sfx(g, SFX_LAND, 0.6f, en->x);
    } else if (t == T_WATER || ny > lv->h * TS + 20) {
        if (t == T_WATER)
            for (int k = 0; k < 3; k++)
                add_particle(g, en->x + (k - 1) * 6, (float)(ty * TS), (k - 1) * 28.0f, -55.0f, 0.4f,
                             &g->splash, 1, k ? 0 : SPR_FLIP);
        en->falling = false;
        en->dead_t = 0;
    } else {
        en->y = ny;
    }
}

static void update_things(Game *g, float dt)
{
    Player *p = &g->p;
    g->coin_t -= dt;
    for (int i = 0; i < g->ncoins; i++) {
        Coin *c = &g->coins[i];
        if (c->taken) continue;
        if (overlap(p->x, p->y, PW, PH, c->x - 5, c->y - 6, 10, 12)) {
            c->taken = true;
            g->coins_got++;
            g->coin_combo = g->coin_t > 0 && g->coin_combo < 10 ? g->coin_combo + 1 : 0;
            g->coin_t = 0.7f;
            snd_play(SFX_COIN, 1.0f, 1.0f + 0.06f * g->coin_combo, pan_at(g, c->x));
            for (int k = 0; k < 4; k++) {
                float ang = k * 1.5708f + 0.785f;
                add_particle(g, c->x, c->y, cosf(ang) * 50, sinf(ang) * 50, 0.3f, &g->sparkle, 1, 0);
            }
        }
    }

    for (int i = 0; i < g->nenemies; i++) {
        Enemy *en = &g->enemies[i];
        if (!en->alive) { update_dead(g, en, dt); continue; }
        update_enemy(g, en, dt);
        float ex, ey, ew, eh;
        enemy_box(g, en, &ex, &ey, &ew, &eh);
        if (!en->seen && en->x > g->camx + 40 && en->x < g->camx + VIEW_W - 40 &&
            ey + eh > g->camy && ey < g->camy + VIEW_H) {
            en->seen = true;
            if (en->kind == EN_GHOST) sfx(g, SFX_GHOST, 1.0f, en->x);
        }
        if (g->state != ST_PLAY || !overlap(p->x, p->y, PW, PH, ex, ey, ew, eh)) continue;
        if (p->vy > 40.0f && p->y + PH - ey < 9.0f) {
            en->alive = false;
            en->dead_t = 0.5f;
            if (g->anim[en->kind].n[AN_SPLAT]) {
                int wx, wy;
                enemy_frame(g, en, &wx, &wy, &en->dead_flags);
                en->die_t = 0;
                en->dead_t = 1.1f;
                if (en->kind == EN_BAT || en->kind == EN_FISH || en->kind == EN_JELLY) {
                    en->y = enemy_y(en);
                    en->vy = -40.0f;
                    en->falling = true;
                }
            } else if (en->kind == EN_GHOST)
                add_particle(g, en->x, ey + eh / 2, 0, -20, 0.4f, &g->ghostpoof, 1, 0);
            else if (en->kind != EN_SHROOM || !g->shroomflat)
                add_particle(g, en->x, ey + eh / 2, 0, -15, 0.35f, g->puff, 2, 0);
            sfx(g, SFX_STOMP, 1.0f, en->x);
            sfx(g, SFX_POOF, en->kind == EN_SHROOM ? 0.5f : 1.0f, en->x);
            p->vy = -STOMP_VEL;
            p->jbuf = 0;
            float b = KILL_BONUS[en->kind];
            g->kills++;
            g->bonus += b;
            g->bonus_t = 0.6f;
            for (int k = 0; k < MAX_POPS; k++)
                if (g->pop[k].t <= 0) {
                    g->pop[k] = (Pop){ en->x, ey - 4, 1.0f, "" };
                    snprintf(g->pop[k].txt, sizeof g->pop[k].txt, "-%.1fS", b);
                    break;
                }
        } else {
            hurt(g, en->x);
        }
    }

    if (g->has_goal && g->state == ST_PLAY &&
        overlap(p->x, p->y, PW, PH, g->goal_tx * TS, g->goal_ty * TS - 8, TS, TS + 8)) {
        g->state = ST_CLEAR;
        g->state_t = 0;
        sfx(g, SFX_CLEAR, 1.0f, p->x);
        float gx = g->goal_tx * TS + TS / 2.0f, gy = (float)(g->goal_ty * TS + TS) - 18.0f;
        for (int k = 0; k < 16; k++) {      /* a ring of sparkles from the doorway */
            float ang = k * 0.3927f;
            add_particle(g, gx, gy, cosf(ang) * 90.0f, sinf(ang) * 90.0f, 0.6f, &g->sparkle, 1, 0);
        }
    }

    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *sh = &g->shot[i];
        if (!sh->live) continue;
        sh->t += dt;
        sh->vy += SHOT_G * dt;
        sh->x += sh->vx * dt;
        sh->y += sh->vy * dt;
        int t = level_at(&g->st.lv, (int)floorf(sh->x / TS), (int)floorf(sh->y / TS));
        bool hit = g->state == ST_PLAY && overlap(p->x, p->y, PW, PH, sh->x - 5, sh->y - 4, 10, 8);
        if (hit) hurt(g, sh->x - sh->vx);
        if (hit || t == T_SOLID || (t == T_WATER && sh->vy > 0) || sh->t > 3.0f) {
            sh->live = false;
            if (g->wsplash[0]) add_particle(g, sh->x, sh->y + 6, 0, 0, 0.32f, g->wsplash, 2, sh->vx < 0 ? SPR_FLIP : 0);
            else add_particle(g, sh->x, sh->y, 0, -30.0f, 0.3f, &g->splash, 1, 0);
        }
    }
    /* the exit: lights drift up out of the doorway; more once every coin is in */
    if (g->has_goal) {
        float gx = g->goal_tx * TS + TS / 2.0f, gy = (float)(g->goal_ty * TS + TS);
        bool ready = g->coins_got == g->ncoins;
        g->mote_t -= dt;
        if (g->mote_t <= 0 && fabsf(gx - (g->camx + VIEW_W / 2.0f)) < VIEW_W) {
            g->mote_t = ready ? 0.035f : 0.08f;
            for (int i = 0; i < MAX_MOTES; i++) {
                Mote *m = &g->mote[i];
                if (m->life > 0) continue;
                static const uint32_t C[4] = { 0xffd860, 0xfff4c0, 0xffa040, 0xffffff };
                *m = (Mote){ gx + (frand(g) - 0.5f) * 52.0f, gy - frand(g) * 30.0f, (frand(g) - 0.5f) * 8.0f,
                             -12.0f - frand(g) * 16.0f - (ready ? 10.0f : 0.0f), 0, 1.2f + frand(g) * 1.0f,
                             frand(g) * 6.28f, C[(int)(frand(g) * 4) & 3] };
                break;
            }
        }
    }
    for (int i = 0; i < MAX_MOTES; i++) {
        Mote *m = &g->mote[i];
        if (m->life <= 0) continue;
        m->t += dt;
        m->x += (m->vx + sinf(m->t * 3.0f + m->ph) * 7.0f) * dt;
        m->y += m->vy * dt;
        if (m->t >= m->life) m->life = 0;
    }

    g->bonus_t -= dt;
    for (int i = 0; i < MAX_POPS; i++) {
        g->pop[i].t -= dt;
        g->pop[i].y -= 22.0f * dt;
    }

    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *q = &g->part[i];
        if (q->life <= 0) continue;
        q->t += dt;
        q->x += q->vx * dt;
        q->y += q->vy * dt;
        if (q->t >= q->life) q->life = 0;
    }
}

static void update_camera(Game *g, float dt)
{
    Player *p = &g->p;
    float look = p->face * 40.0f * fminf(1.0f, fabsf(p->vx) / WALK_SPEED);
    float tx = p->x + PW / 2.0f - VIEW_W / 2.0f + look;
    float ty = p->y + PH / 2.0f - VIEW_H * 0.55f;
    g->camx += (tx - g->camx) * fminf(1.0f, dt * 6.0f);
    g->camy += (ty - g->camy) * fminf(1.0f, dt * 4.0f);
    float mx = (float)(g->st.lv.w * TS - VIEW_W), my = (float)(g->st.lv.h * TS - VIEW_H);
    if (g->camx > mx) g->camx = mx;
    if (g->camx < 0) g->camx = 0;
    if (g->camy > my) g->camy = my;
    if (g->camy < 0) g->camy = 0;
}

void game_update(Game *g, const Actions *a, float dt)
{
    g->time += dt;
    g->state_t += dt;
    if (g->state == ST_PLAY) {
        if (a->left || a->right || a->jump || a->jump_pressed) g->clock_on = true;
        if (g->clock_on) g->level_time += dt;
        update_player(g, a, dt);
    } else if (g->state == ST_DEAD) {
        g->p.vy += GRAVITY * 0.5f * dt;
        g->p.y += g->p.vy * dt * 0.5f;
        if (g->state_t > 1.6f) game_restart(g);
    } else if (g->state == ST_CLEAR) {      /* the results screen (main.c) takes over */
        Actions none = { 0 };
        update_player(g, &none, dt);
    }
    update_things(g, dt);
    update_camera(g, dt);
}

void game_teleport(Game *g, int tx)
{
    for (int ty = 1; ty < g->st.lv.h; ty++)       /* footing with room above it, not a cave's roof */
        if (standable(level_at(&g->st.lv, tx, ty)) && level_at(&g->st.lv, tx, ty - 1) == T_EMPTY) {
            spawn_player(g, (float)(tx * TS + (TS - PW) / 2), (float)(ty * TS - PH));
            g->practice = true;
            g->camx = g->p.x - VIEW_W / 2.0f;
            g->camy = g->p.y - VIEW_H * 0.55f;
            update_camera(g, 0);
            return;
        }
}

void game_info(const Game *g, GameInfo *o)
{
    o->x = g->p.x; o->y = g->p.y; o->vx = g->p.vx; o->vy = g->p.vy;
    o->ground = g->p.ground;
    o->hearts = g->hearts;
    o->coins = g->coins_got;
    o->coins_total = g->ncoins;
    o->level = g->level;
    o->level_title = g->st.def->title;
    o->state = g->state == ST_PLAY ? "PLAY" : g->state == ST_DEAD ? "DEAD" : "CLEAR";
    o->clear = g->state == ST_CLEAR;
    o->clear_t = g->state == ST_CLEAR ? g->state_t : 0;
    o->time = g->level_time;
    o->bonus = g->bonus;
    o->kills = g->kills;
    o->practice = g->practice;
    o->level_hash = g->level_hash;
}

float game_final_time(float time, float bonus) { return time > bonus ? time - bonus : 0; }

void game_fmt_time(char *buf, size_t n, float secs)
{
    int cs = (int)(secs * 100.0f + 0.5f);
    if (cs < 0) cs = 0;
    snprintf(buf, n, "%d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
}

/* ------------------------------------------------------------ rendering */

static const char *HEART[8] = {
    ".kkk.kkk.",
    "krrrkrrrk",
    "krwrrrrrk",
    "krrrrrrrk",
    ".krrrrrk.",
    "..krrrk..",
    "...krk...",
    "....k....",
};

static void draw_heart(Canvas *cv, int x, int y, bool full)
{
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 9; i++) {
            char c = HEART[j][i];
            uint32_t col = c == 'k' ? 0x1a0c1e : c == 'w' ? (full ? 0xffd0d8 : 0x5a3a60)
                         : c == 'r' ? (full ? 0xe8303c : 0x3a2440) : 0;
            if (c != '.') {
                gfx_fill(cv, x + i * 2, y + j * 2, 2, 2, col);
            }
        }
}

static void draw_mist(Canvas *cv, int camx, float t, int y0, int h, float speed, int strength)
{
    for (int y = y0; y < y0 + h; y++) {
        if ((unsigned)y >= (unsigned)cv->h) continue;
        float fy = (float)(y - y0) / h;
        float band = sinf(fy * 3.14159f);
        for (int x = 0; x < cv->w; x++) {
            float wx = x + camx * speed + t * 6.0f;
            float n = sinf(wx * 0.021f + sinf(wx * 0.007f) * 2.0f) * 0.5f + 0.5f;
            n *= 0.6f + 0.4f * sinf(wx * 0.053f + y * 0.11f);
            int a = (int)(n * band * strength);
            if (a > 0) cv->px[y * cv->w + x] = gfx_mix(cv->px[y * cv->w + x], 0x6a7cc8, a);
        }
    }
}

typedef struct { int tx0, ty0, tx1, ty1; } TileRange;

static TileRange visible_tiles(int cx, int cy)
{
    return (TileRange){ cx / TS - 1, cy / TS - 1, (cx + VIEW_W) / TS + 1, (cy + VIEW_H) / TS + 1 };
}

static void draw_tiles_moon(Game *g, Canvas *cv, int cx, int cy)
{
    const Level *lv = &g->st.lv;
    TileRange r = visible_tiles(cx, cy);
    /* interiors first, then exposed tops so grass overhangs neighbours */
    for (int pass = 0; pass < 2; pass++)
        for (int ty = r.ty0; ty <= r.ty1; ty++)
            for (int tx = r.tx0; tx <= r.tx1; tx++) {
                if (tx < 0 || tx >= lv->w || ty < 0 || ty >= lv->h) continue;
                int t = level_at(lv, tx, ty);
                int sx = tx * TS - cx, sy = ty * TS - cy;
                uint32_t h = hash2(tx, ty);
                if (t == T_SOLID) {
                    bool exposed = level_at(lv, tx, ty - 1) != T_SOLID;
                    if (pass == 0) {
                        /* deeper blocks get plainer and darker */
                        int depth = 0;
                        while (depth < 4 && level_at(lv, tx, ty - depth - 1) == T_SOLID) depth++;
                        int shade = depth <= 1 ? 0 : (depth - 1) * 45;
                        gfx_fill(cv, sx, sy, TS, TS, gfx_mix(0x15173a, 0x07081a, shade));
                        if (!exposed) {
                            int k = (int)(h % 32);
                            int v = depth >= 2 ? (k < 16 ? 0 : k < 24 ? 1 : k < 30 ? 2 : 4)
                                  : (k < 12 ? 0 : k < 18 ? 1 : k < 21 ? 2 : k < 25 ? 3 : k < 29 ? 4 : 5);
                            draw_sprite(cv, g->stone[v], sx + TS / 2, sy + TS,
                                        (h >> 8) & 1 ? SPR_FLIP : 0, 0x07081a, shade);
                        }
                    } else if (exposed) {
                        int k = h % 10;
                        const Sprite *s = g->top[k < 3 ? 0 : k < 6 ? 1 : k < 7 ? 2 : k < 8 ? 3 : k < 9 ? 4 : 5];
                        draw_sprite(cv, s, sx + TS / 2, sy + TS, (h >> 8) & 1 ? SPR_FLIP : 0, 0, 0);
                    }
                } else if (t == T_SPIKE && pass == 1) {
                    draw_sprite(cv, g->spike, sx + TS / 2, sy + TS, 0, 0, 0);
                }
            }
}

/* sunset lake: mortar fill with scattered cobbles, outlined sides, grass caps */
static void draw_tiles_lake(Game *g, Canvas *cv, int cx, int cy)
{
    const Level *lv = &g->st.lv;
    const uint32_t MORTAR = 0x2b1a23, DEEP = 0x120a10, EDGE = 0x140a10, RIM = 0x3e232c;
    TileRange r = visible_tiles(cx, cy);
    for (int pass = 0; pass < 2; pass++)
        for (int ty = r.ty0; ty <= r.ty1; ty++)
            for (int tx = r.tx0; tx <= r.tx1; tx++) {
                if (tx < 0 || tx >= lv->w || ty < 0 || ty >= lv->h) continue;
                if (level_at(lv, tx, ty) != T_SOLID) continue;
                int sx = tx * TS - cx, sy = ty * TS - cy;
                uint32_t h = hash2(tx, ty);
                bool up = level_at(lv, tx, ty - 1) == T_SOLID;
                bool left = tx > 0 && level_at(lv, tx - 1, ty) == T_SOLID;
                bool right = tx < lv->w - 1 && level_at(lv, tx + 1, ty) == T_SOLID;
                bool down = ty >= lv->h - 1 || level_at(lv, tx, ty + 1) == T_SOLID;
                int depth = 0;
                while (depth < 5 && level_at(lv, tx, ty - depth - 1) == T_SOLID) depth++;
                int shade = depth <= 1 ? 0 : (depth - 1) * 38;
                if (pass == 0) {
                    gfx_fill(cv, sx, sy, TS, TS, gfx_mix(MORTAR, DEEP, shade));
                    /* cobbles may spill into solid neighbours but never into the air */
                    sprite_clip(left ? sx - 8 : sx + 1, up ? sy - 8 : sy,
                                right ? sx + TS + 8 : sx + TS - 1, down ? sy + TS + 8 : sy + TS - 1);
                    /* two cobbles on one diagonal, sometimes a third */
                    bool diag = (h >> 3) & 1;
                    for (int k = 0; k < 3; k++) {
                        uint32_t hk = hash2(tx * 3 + k, ty + 7777);
                        if (k == 2 && hk % 10 >= 4) continue;
                        int qx = k == 2 ? 8 : (k ^ diag) ? 12 : 4, qy = k == 2 ? (diag ? 12 : 4) : k ? 12 : 4;
                        if (k == 2) qx = diag ? 4 : 12;
                        int px = sx + qx + (int)(hk >> 4) % 3 - 1;
                        int py = sy + qy + (int)(hk >> 6) % 3 - 1;
                        draw_sprite(cv, pick(&g->cob, hk >> 8), px, py, (hk >> 12) & 1 ? SPR_FLIP : 0,
                                    DEEP, shade);
                    }
                    sprite_noclip();
                } else {
                    uint32_t ec = gfx_mix(EDGE, DEEP, shade / 2), rc = gfx_mix(RIM, DEEP, shade);
                    if (!left)  { gfx_fill(cv, sx, sy, 1, TS, ec); gfx_fill(cv, sx + 1, sy, 1, TS, rc); }
                    if (!right) { gfx_fill(cv, sx + TS - 1, sy, 1, TS, ec); gfx_fill(cv, sx + TS - 2, sy, 1, TS, rc); }
                    if (!down)  { gfx_fill(cv, sx, sy + TS - 1, TS, 1, ec); gfx_fill(cv, sx + 1, sy + TS - 2, TS - 2, 1, rc); }
                    if (!up) {
                        const Sprite *s = !left && g->capl ? g->capl : !right && g->capr ? g->capr : pick(&g->cap, h);
                        draw_sprite(cv, s, sx + TS / 2, sy, 0, 0, 0);
                    }
                }
            }
}

/* tropical island: flat sand with scattered speckles, edge strips cut from a
 * painted pillar, grass caps on top. Every piece is anchored to the tile's
 * top-left corner, so they line up with any shape of ground. */
static void draw_tiles_island(Game *g, Canvas *cv, int cx, int cy)
{
    const Level *lv = &g->st.lv;
    TileRange r = visible_tiles(cx, cy);
    for (int pass = 0; pass < 3; pass++)
        for (int ty = r.ty0; ty <= r.ty1; ty++)
            for (int tx = r.tx0; tx <= r.tx1; tx++) {
                if (tx < 0 || tx >= lv->w || ty < 0 || ty >= lv->h) continue;
                if (level_at(lv, tx, ty) != T_SOLID) continue;
                int sx = tx * TS - cx, sy = ty * TS - cy;
                uint32_t h = hash2(tx, ty);
                bool up = level_at(lv, tx, ty - 1) == T_SOLID;
                bool left = tx > 0 && level_at(lv, tx - 1, ty) == T_SOLID;
                bool right = tx < lv->w - 1 && level_at(lv, tx + 1, ty) == T_SOLID;
                bool down = ty >= lv->h - 1 || level_at(lv, tx, ty + 1) == T_SOLID;
                /* no depth shading: on bright sand it shows as bands and columns */
                if (pass == 0) {
                    gfx_fill(cv, sx, sy, TS, TS, g->sand);
                    sprite_clip(left ? sx - 6 : sx + 10, up ? sy - 6 : sy + 12,      /* not under the grass */
                                right ? sx + TS + 6 : sx + TS - 6, down ? sy + TS + 6 : sy + TS - 7);
                    for (int k = 0; k < 2; k++) {
                        uint32_t hk = hash2(tx * 2 + k, ty + 5151);
                        if (k == 1 && hk % 10 >= 5) continue;
                        int px = sx + 3 + (int)(hk >> 4) % 10, py = sy + 3 + (int)(hk >> 9) % 10;
                        draw_sprite(cv, pick(&g->dot, hk >> 13), px, py, 0, 0, 0);
                    }
                    sprite_noclip();
                } else if (pass == 1) {
                    if (!left)  draw_sprite(cv, pick(&g->sidel, h), sx, sy, 0, 0, 0);
                    if (!right) draw_sprite(cv, pick(&g->sider, h >> 3), sx, sy, 0, 0, 0);
                    if (!down)
                        draw_sprite(cv, !left ? g->botl : !right ? g->botr : pick(&g->bot, h >> 5), sx, sy, 0, 0, 0);
                    if (!down && !left && !right) draw_sprite(cv, g->botr, sx, sy, 0, 0, 0);
                } else if (!up) {
                    if (!left) draw_sprite(cv, g->capl, sx, sy, 0, 0, 0);
                    if (!right) draw_sprite(cv, g->capr, sx, sy, 0, 0, 0);
                    if (left && right) draw_sprite(cv, pick(&g->cap, h), sx, sy, 0, 0, 0);
                }
            }
}

/* an island '=' run: end pieces and middle slices of one painted platform */
static void draw_plat_island(Game *g, Canvas *cv, const PlatRun *pr, int cx, int cy)
{
    int x0 = pr->tx * TS - cx, y = pr->ty * TS - cy;
    if (x0 > VIEW_W + 20 || x0 + pr->len * TS < -20) return;
    for (int i = 1; i < pr->len - 1; i++)
        draw_sprite(cv, pick(&g->pm, hash2(pr->tx + i, pr->ty)), x0 + i * TS, y, 0, 0, 0);
    draw_sprite(cv, g->pl, x0, y, 0, 0, 0);
    draw_sprite(cv, g->pr, x0 + (pr->len - 1) * TS, y, 0, 0, 0);
}

/* crystal caverns: a seamless cobble texture laid in world space, so the rock
 * has no tile seams. Exposed sides and undersides are carved into a row of
 * bulging stones (notches between them), outlined and darkened toward the
 * edge; exposed tops get mossy caps. '=' ledges are drawn the same way. */
#define CAVE_TOP 4          /* rock starts this far below an exposed top: the cap covers it */

static bool rock_at(const Level *lv, int tx, int ty)
{
    if (tx < 0 || tx >= lv->w || ty < 0 || ty >= lv->h) return true;   /* walls, roof and floor go on */
    return lv->tile[ty * lv->w + tx] == T_SOLID || lv->ch[ty * lv->w + tx] == '=';
}

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* how far an exposed edge is pushed in at position p along it: stones ~12px
 * long, round notches 3-4px deep where two meet */
static int edge_notch(int key, int p)
{
    int k = floordiv(p, 12);
#define CUT(k) ((k) * 12 + (int)(hash2(key, (k)) % 7) - 3)
    int b = CUT(k);
    if (p < b) { k--; b = CUT(k); }
    int e = CUT(k + 1);
    if (p >= e) { k++; b = e; e = CUT(k + 1); }
#undef CUT
    float d = 2.0f * (p - b + 0.5f) / (float)(e - b) - 1.0f;
    int deep = 3 + (int)((hash2(key + 7777, k) >> 7) & 1);
    return (int)(deep * (1.0f - sqrtf(fmaxf(0.0f, 1.0f - d * d))) + 0.5f);
}

static bool rock_px(const Level *lv, int wx, int wy)
{
    int tx = floordiv(wx, TS), ty = floordiv(wy, TS);
    if (!rock_at(lv, tx, ty)) return false;
    int i = wx - tx * TS, j = wy - ty * TS;
    bool up = rock_at(lv, tx, ty - 1), dn = rock_at(lv, tx, ty + 1);
    bool lf = rock_at(lv, tx - 1, ty), rt = rock_at(lv, tx + 1, ty);
    if (!up && j < CAVE_TOP) return false;
    if (!lf && i < edge_notch(tx * 2, wy)) return false;
    if (!rt && TS - 1 - i < edge_notch(tx * 2 + 1, wy)) return false;
    if (!dn && TS - 1 - j < edge_notch(0x100000 + ty, wx)) return false;
    if (!dn && !lf && i + (TS - 1 - j) < 4) return false;
    if (!dn && !rt && (TS - 1 - i) + (TS - 1 - j) < 4) return false;
    return true;
}

static void draw_tiles_cave(Game *g, Canvas *cv, int cx, int cy)
{
    const Level *lv = &g->st.lv;
    const Sprite *tex = g->rocktex;
    const Image *im = tex->img;
    const int N = tex->w, M = 3;            /* margin: the edge shading reaches 3px in */
    static const signed char RING[3][12][2] = {
        { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } },
        { { 2, 0 }, { -2, 0 }, { 0, 2 }, { 0, -2 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } },
        { { 3, 0 }, { -3, 0 }, { 0, 3 }, { 0, -3 }, { 2, 1 }, { 2, -1 }, { -2, 1 }, { -2, -1 },
          { 1, 2 }, { 1, -2 }, { -1, 2 }, { -1, -2 } },
    };
    static const int RING_N[3] = { 4, 8, 12 };
    TileRange r = visible_tiles(cx, cy);
    for (int ty = r.ty0; ty <= r.ty1; ty++)
        for (int tx = r.tx0; tx <= r.tx1; tx++) {
            if (tx < 0 || tx >= lv->w || ty < 0 || ty >= lv->h || !rock_at(lv, tx, ty)) continue;
            int sx = tx * TS - cx, sy = ty * TS - cy;
            if (sx <= -TS || sy <= -TS || sx >= cv->w || sy >= cv->h) continue;
            bool closed = true;         /* no carving within reach: a plain copy */
            for (int k = 0; k < 9; k++) closed &= rock_at(lv, tx + k % 3 - 1, ty + k / 3 - 1);
            bool in[TS + 2 * M][TS + 2 * M];
            if (!closed)
                for (int j = 0; j < TS + 2 * M; j++)
                    for (int i = 0; i < TS + 2 * M; i++)
                        in[j][i] = rock_px(lv, tx * TS + i - M, ty * TS + j - M);
            for (int j = 0; j < TS; j++) {
                int y = sy + j;
                if ((unsigned)y >= (unsigned)cv->h) continue;
                int wy = ty * TS + j;
                const uint32_t *row = im->px + (size_t)(tex->y + wy % N) * im->w + tex->x;
                uint32_t *dst = cv->px + (size_t)y * cv->w;
                for (int i = 0; i < TS; i++) {
                    int x = sx + i;
                    if ((unsigned)x >= (unsigned)cv->w) continue;
                    uint32_t c = row[(tx * TS + i) % N] & 0xffffff;
                    if (!closed) {
                        if (!in[j + M][i + M]) continue;
                        int d = 0;
                        for (int k = 0; k < 3 && !d; k++)
                            for (int q = 0; q < RING_N[k]; q++)
                                if (!in[j + M + RING[k][q][1]][i + M + RING[k][q][0]]) { d = k + 1; break; }
                        if (d == 1) c = g->rockedge;
                        else if (d == 2) c = gfx_mix(c, g->rockrim, 170);
                        else if (d == 3) c = gfx_mix(c, g->rockrim, 80);
                    }
                    dst[x] = c;
                }
            }
        }
    /* caps over the tops */
    for (int ty = r.ty0; ty <= r.ty1; ty++)
        for (int tx = r.tx0; tx <= r.tx1; tx++) {
            if (tx < 0 || tx >= lv->w || ty < 0 || ty >= lv->h) continue;
            if (!rock_at(lv, tx, ty) || rock_at(lv, tx, ty - 1)) continue;
            int sx = tx * TS - cx, sy = ty * TS - cy;
            bool lf = rock_at(lv, tx - 1, ty), rt = rock_at(lv, tx + 1, ty);
            if (!lf) draw_sprite(cv, g->capl, sx, sy, 0, 0, 0);
            if (!rt) draw_sprite(cv, g->capr, sx, sy, 0, 0, 0);
            if (lf && rt) draw_sprite(cv, pick(&g->cap, hash2(tx, ty)), sx, sy, 0, 0, 0);
        }
}

/* see_through: blend over what is behind (the underwater scene) instead of painting */
typedef struct { uint32_t top, deep, crest, crest2, under, glint; bool see_through; } WaterLook;
static const WaterLook WATER_SUNSET = { 0x4b3f7c, 0x1e1838, 0xffc890, 0xe89070, 0xb0607a, 0xf08850, false };
static const WaterLook WATER_TROPIC = { 0x2a9cd0, 0x0c3c78, 0xf4ffff, 0x9ce8f4, 0x58c8e0, 0xc8f6ff, true };
static const WaterLook WATER_CAVE   = { 0x2470c8, 0x081640, 0xc8f8ff, 0x62c8f4, 0x44a8ec, 0x8ee0ff, false };

static void draw_water(Game *g, Canvas *cv, int cx, int cy, float t)
{
    Theme th = g->st.def->theme;
    const WaterLook *wl = th == TH_ISLAND ? &WATER_TROPIC : th == TH_CAVE ? &WATER_CAVE : &WATER_SUNSET;
    const Level *lv = &g->st.lv;
    TileRange r = visible_tiles(cx, cy);
    for (int ty = r.ty0; ty <= r.ty1; ty++)
        for (int tx = r.tx0; tx <= r.tx1; tx++) {
            if (level_at(lv, tx, ty) != T_WATER) continue;
            int surf = ty;
            while (level_at(lv, tx, surf - 1) == T_WATER) surf--;
            int sx = tx * TS - cx, sy = ty * TS - cy;
            for (int j = 0; j < TS; j++) {
                int y = sy + j;
                if ((unsigned)y >= (unsigned)cv->h) continue;
                int wy = ty * TS + j, depth = wy - surf * TS;
                uint32_t base = gfx_mix(wl->top, wl->deep, depth * 5 > 256 ? 256 : depth * 5);
                for (int i = 0; i < TS; i++) {
                    int x = sx + i;
                    if ((unsigned)x >= (unsigned)cv->w) continue;
                    int wx = tx * TS + i;
                    uint32_t c = wl->see_through ? gfx_mix(cv->px[y * cv->w + x], base, 40 + (depth > 70 ? 70 : depth))
                                                 : base;
                    float wave = sinf(wx * 0.11f + t * 2.2f) + sinf(wx * 0.047f - t * 1.3f);
                    if (depth == 0) c = wave > 0.6f ? wl->crest : wl->crest2;
                    else if (depth == 1) c = gfx_mix(wl->under, base, 80);
                    else {
                        /* drifting sunset glints */
                        float s = sinf(wx * 0.09f + wy * 1.7f + t * 1.1f) * sinf(wx * 0.023f - t * 0.4f + wy * 0.3f);
                        if (s > 0.55f) c = gfx_mix(c, wl->glint, 150 - depth * 2 > 30 ? 150 - depth * 2 : 30);
                    }
                    cv->px[y * cv->w + x] = c;
                }
            }
        }
}

/* '%' cells: non-solid ruined masonry behind the play layer */
static void draw_backwall(Game *g, Canvas *cv, int cx, int cy)
{
    const Level *lv = &g->st.lv;
    const uint32_t TO = 0x0f1336;
    const int SH = 175;
    TileRange r = visible_tiles(cx, cy);
    for (int pass = 0; pass < 2; pass++)
        for (int ty = r.ty0; ty <= r.ty1; ty++)
            for (int tx = r.tx0; tx <= r.tx1; tx++) {
                if (level_ch(lv, tx, ty) != '%') continue;
                int sx = tx * TS - cx, sy = ty * TS - cy;
                uint32_t h = hash2(tx, ty + 1000);
                char up = level_ch(lv, tx, ty - 1);
                bool exposed = up != '%' && up != '#';
                int flags = (h >> 8) & 1 ? SPR_FLIP : 0;
                if (pass == 0) {
                    gfx_fill(cv, sx, sy, TS, TS, gfx_mix(0x15173a, TO, SH));
                    if (!exposed)
                        draw_sprite(cv, g->stone[h % 8 < 5 ? 0 : h % 8 < 7 ? 1 : 2],
                                    sx + TS / 2, sy + TS, flags, TO, SH);
                } else if (exposed) {
                    draw_sprite(cv, g->top[h % 3 ? 0 : 1], sx + TS / 2, sy + TS, flags, TO, SH);
                }
            }
}

/* a sagging rope from (x0, y0) to (x1, y1) */
static void draw_rope(Canvas *cv, int x0, int y0, int x1, int y1, float sag, uint32_t col, uint32_t dark)
{
    for (int x = x0; x <= x1; x++) {
        float u = x1 > x0 ? (float)(x - x0) / (x1 - x0) : 0;
        int y = (int)lroundf(y0 + (y1 - y0) * u + sag * 4 * u * (1 - u));
        gfx_put(cv, x, y, col);
        gfx_put(cv, x, y + 1, dark);
    }
}

/* dock posts: screen x of each, every ~4 tiles with one at both ends */
static int dock_posts(const Span *sp, int x0, int x1, int *px, int max)
{
    int np = sp->len / 4 + 1;
    if (np > max - 1) np = max - 1;
    for (int k = 0; k <= np; k++) px[k] = x0 + 4 + (x1 - x0 - 8) * k / np;
    return np + 1;
}

/* world y where a dock post at world x meets something: water, ground or a platform */
static int post_base(const Game *g, int wx, int ty, bool *water)
{
    const Level *lv = &g->st.lv;
    int tx = wx / TS;
    for (int y = ty + 1; y < lv->h; y++) {
        int t = level_at(lv, tx, y);
        if (t != T_EMPTY) { *water = t == T_WATER; return y * TS; }
    }
    *water = false;
    return -1;
}

/* island pier: rope swags, posts reaching down into the water, the beam on top */
static void draw_dock(Game *g, Canvas *cv, const Span *sp, int x0, int x1, int top, int cx, int cy)
{
    int px[64];
    int n = dock_posts(sp, x0, x1, px, 64);
    for (int k = 0; k + 1 < n; k++)
        draw_rope(cv, px[k], top - 11, px[k + 1], top - 11, 5.0f + (px[k + 1] - px[k]) * 0.03f, 0x904b2d, 0x4a2410);
    for (int k = 0; k < n; k++) {
        bool water;
        int base = post_base(g, px[k] + cx, sp->ty, &water);
        int foot = top - g->dpost->ay + g->dpost->h;
        if (base >= 0)
            for (int y = foot; y < base - cy + (water ? 2 : 0); y++) draw_sprite(cv, g->dshaft, px[k], y, 0, 0, 0);
        draw_sprite(cv, g->dpost, px[k], top, 0, 0, 0);
    }
    draw_hstrip(cv, g->dbeam, x0, x1, top, 0, 0);
    draw_sprite(cv, g->dend, x0, top, 0, 0, 0);
    draw_sprite(cv, g->dend, x1, top, SPR_FLIP, 0, 0);
}

/* the water ring round each dock post, drawn after the water */
static void draw_dock_splashes(Game *g, Canvas *cv, int cx, int cy, float t)
{
    if (!g->dsplash) return;
    for (int i = 0; i < g->nspan; i++) {
        const Span *sp = &g->span[i];
        if (sp->kind != SP_BRIDGE) continue;
        int x0 = sp->tx * TS - cx, x1 = (sp->tx + sp->len) * TS - cx;
        if (x1 < -40 || x0 > VIEW_W + 40) continue;
        int px[64];
        int n = dock_posts(sp, x0, x1, px, 64);
        for (int k = 0; k < n; k++) {
            bool water;
            int base = post_base(g, px[k] + cx, sp->ty, &water);
            if (water)
                draw_sprite(cv, g->dsplash, px[k], base - cy + 1, ((int)(t * 2.5f) + k) & 1 ? SPR_FLIP : 0, 0, 0);
        }
    }
}

static void draw_spans(Game *g, Canvas *cv, int cx, int cy)
{
    for (int i = 0; i < g->nspan; i++) {
        const Span *sp = &g->span[i];
        int x0 = sp->tx * TS - cx, x1 = (sp->tx + sp->len) * TS - cx;
        if (x1 < -40 || x0 > VIEW_W + 40) continue;
        int top = sp->ty * TS - cy;
        if (sp->kind == SP_BRIDGE && g->st.def->theme == TH_ISLAND) {
            draw_dock(g, cv, sp, x0, x1, top, cx, cy);
        } else if (sp->kind == SP_BRIDGE) {
            int foot = top + 12;
            int np = sp->len / 4 + 1;               /* posts every ~4 tiles, both ends */
            int px[64];
            for (int k = 0; k <= np && k < 64; k++) px[k] = x0 + 3 + (x1 - x0 - 6) * k / np;
            for (int k = 0; k < np && k < 63; k++)
                draw_rope(cv, px[k], top - 12, px[k + 1], top - 12, 4.0f + (px[k + 1] - px[k]) * 0.04f,
                          0x8a4a22, 0x3a1810);
            draw_hstrip(cv, g->planks, x0, x1, top - 1, 0, 0);
            for (int k = 0; k <= np && k < 64; k++) {
                draw_sprite(cv, g->bpost, px[k], foot, 0, 0, 0);
                draw_sprite(cv, g->rpost, px[k], top + 3, 0, 0, 0);     /* handrail post */
            }
        } else {
            int foot = top + TS;
            const Sprite *post = sp->kind == SP_RAIL ? g->fpost : g->rpost;
            int np = (sp->len + 1) / 2;
            int px[64];
            for (int k = 0; k <= np && k < 64; k++) px[k] = x0 + 4 + (x1 - x0 - 8) * k / np;
            if (sp->kind == SP_RAIL)
                draw_hstrip(cv, g->rail, px[0], px[np < 63 ? np : 63], foot - 13, 0, 0);
            else
                for (int k = 0; k < np && k < 63; k++)
                    draw_rope(cv, px[k], foot - 14, px[k + 1], foot - 14, 3.0f, 0x8a4a22, 0x3a1810);
            for (int k = 0; k <= np && k < 64; k++) draw_sprite(cv, post, px[k], foot, 0, 0, 0);
        }
    }
}

static void draw_player(Game *g, Canvas *cv, int cx, int cy)
{
    Player *p = &g->p;
    if (p->inv_t > 0 && g->state == ST_PLAY && ((int)(p->inv_t * 14) & 1)) return;
    const Sprite *s;
    if (g->state == ST_DEAD || p->hurt_t > 0) s = g->b_hurt;
    else if (!p->ground) s = p->vy < -60 ? g->b_jump : p->vy < 80 ? g->b_fall[1] : g->b_fall[0];
    else if (p->land_t > 0) s = g->b_land;
    else if (fabsf(p->vx) > 5) {
        bool skid = (p->vx > 0) != (p->face > 0) && fabsf(p->vx) > 60;
        s = skid ? g->b_skid : g->b_run[(int)(p->anim / 9.0f) % 6];
    } else {
        s = fmodf(p->idle_t, 3.0f) > 2.75f ? g->b_idle[1] : g->b_idle[0];
    }
    int flags = p->face < 0 ? SPR_FLIP : 0;
    if (s == g->b_skid) flags ^= SPR_FLIP;
    draw_sprite(cv, s, (int)floorf(p->x + PW / 2.0f + 0.5f) - cx,
                (int)floorf(p->y + PH + 0.5f) - cy, flags, 0, 0);
}

static void draw_enemies(Game *g, Canvas *cv, int cx, int cy)
{
    for (int i = 0; i < g->nenemies; i++) {
        const Enemy *en = &g->enemies[i];
        int x = (int)en->x - cx;
        if (x < -40 || x > VIEW_W + 40) continue;
        if (has_anim(g, en->kind)) {
            const Anim *an = &g->anim[en->kind];
            int wx, wy, fl;
            const Sprite *s;
            if (en->alive) {
                s = enemy_frame(g, en, &wx, &wy, &fl);
            } else {
                if (!an->n[AN_SPLAT] || (en->dead_t <= 0 && !en->falling)) continue;
                if (!en->falling && en->dead_t < 0.3f && ((int)(en->dead_t * 16) & 1)) continue;   /* blink out */
                fl = en->dead_flags;
                wx = (int)en->x; wy = (int)en->y;
                if (en->falling)
                    s = anim_get(an, AN_HIT, (int)(en->die_t * 12));
                else if (en->die_t < 0.18f)
                    s = anim_get(an, AN_HIT, (int)(en->die_t / 0.06f));
                else
                    s = anim_get(an, AN_SPLAT, (int)((en->die_t - 0.18f) / 0.1f));
            }
            draw_sprite(cv, s, wx - cx, wy - cy, fl, 0, 0);
            continue;
        }
        if (!en->alive) {
            if (en->kind == EN_SHROOM && en->dead_t > 0)
                draw_sprite(cv, g->shroomflat, x, (int)en->y - cy, 0, 0, 0);
            continue;
        }
        switch (en->kind) {
        case EN_GHOST: {
            int f = (int)(en->t * 6) % 4;
            float gy = enemy_y(en);
            gfx_glow(cv, x, (int)gy - cy, 18, 0x4050a0, 70);
            draw_sprite(cv, g->ghost[f], x, (int)gy - cy, en->vx > 0 ? SPR_FLIP : 0, 0, 0);
            break;
        }
        case EN_SHROOM: {
            static const int SEQ[4] = { 0, 1, 2, 1 };
            int k = (int)(en->t * 7) % 4;
            draw_sprite(cv, g->shroom[SEQ[k]], x, (int)en->y - cy - (k & 1), en->vx > 0 ? SPR_FLIP : 0, 0, 0);
            break;
        }
        case EN_SNAIL: {
            int k = (int)(en->t * 2.5f) % 2;
            draw_sprite(cv, g->snail[k], x, (int)en->y - cy, en->vx > 0 ? SPR_FLIP : 0, 0, 0);
            break;
        }
        case EN_CRAB: {
            int k = (int)(en->t * 8) % 4;
            draw_sprite(cv, g->crab[k], x, (int)en->y - cy, 0, 0, 0);
            break;
        }
        case EN_JELLY: {
            int k = (int)(en->t * 5) % 4;
            draw_sprite(cv, g->jelly[k], x, (int)enemy_y(en) - cy, 0, 0, 0);
            break;
        }
        case EN_BIRD: {
            int k = (int)(en->t * 9) % 4;
            draw_sprite(cv, g->bird[k], x, (int)enemy_y(en) - cy, en->vx < 0 ? SPR_FLIP : 0, 0, 0);
            break;
        }
        default: break;
        }
    }
}

static void draw_glows(Game *g, Canvas *cv, int cx, int cy, float t)
{
    for (int i = 0; i < g->ndeco; i++) {
        const Deco *d = &g->deco[i];
        if (!d->glow_dy) continue;
        int lx = d->x - cx + (d->flags & SPR_FLIP ? -d->glow_dx : d->glow_dx), ly = d->y - cy + d->glow_dy;
        if (lx < -60 || lx > VIEW_W + 60) continue;
        if (d->glow_c) {        /* crystals: a slow cold pulse */
            float pu = 0.8f + 0.2f * sinf(t * 1.7f + d->x * 0.13f);
            gfx_glow(cv, lx, ly, 34, d->glow_c, (int)(80 * pu));
            gfx_glow(cv, lx, ly, 10, 0xa0d8ff, (int)(70 * pu));
            continue;
        }
        float fl = 0.85f + 0.15f * sinf(t * 13 + d->x) * sinf(t * 7.3f + d->y);
        gfx_glow(cv, lx, ly, 44, 0xffa040, (int)(95 * fl));
        gfx_glow(cv, lx, ly, 12, 0xffd080, (int)(120 * fl));
    }
}

static void render_moon_back(Game *g, Canvas *cv, int cx, int cy, int below, float t)
{
    int levw = g->st.lv.w * TS, levh = g->st.lv.h * TS;
    const Image *far = &g->st.far;
    /* far landscape spans the whole level exactly once */
    float fx = levw > VIEW_W ? (float)(far->w - VIEW_W) / (levw - VIEW_W) : 0;
    if (fx > 0.5f) fx = 0.5f;
    float vy = levh > VIEW_H ? (float)cy / (levh - VIEW_H) : 1.0f;
    int foy = (int)((far->h - VIEW_H) * vy);
    draw_layer(cv, far, (int)(cx * fx), foy, 0, 0, 0);
    draw_mist(cv, cx, t, 150 - (foy - (far->h - VIEW_H)), 50, 0.2f, 70);
    /* mid-screen forest: the tree line cut from the background (misty, far)
     * then the pine strip in front of it */
    const int TREE_TOP = 182;               /* first tree row in bg_trees.png */
    const int PINE_TOP = 80;                /* first tree row in bg_pines.png */
    draw_layer(cv, &g->st.mid[0], (int)(cx * 0.42f) + 517, TREE_TOP - 88 + (int)(below * 0.2f),
               LAYER_WRAP | LAYER_ALPHA | LAYER_EXTEND, 0x1c2860, 90);
    draw_mist(cv, cx, t * 1.3f, 128 - below / 5, 40, 0.5f, 55);
    draw_layer(cv, &g->st.mid[1], (int)(cx * 0.72f) + 200, PINE_TOP - 92 + (int)(below * 0.35f),
               LAYER_WRAP | LAYER_ALPHA | LAYER_EXTEND, 0, 0);
    draw_mist(cv, cx, t * 1.8f, 180 - below / 3, 40, 0.85f, 40);
    draw_backwall(g, cv, cx, cy);
}

static void render_lake_back(Game *g, Canvas *cv, int cx, int below)
{
    int levw = g->st.lv.w * TS;
    const Image *far = &g->st.far, *lake = &g->st.mid[0];
    float fx = levw > VIEW_W ? (float)(far->w - VIEW_W) / (levw - VIEW_W) : 0;
    if (fx > 0.5f) fx = 0.5f;
    const int SUN_Y = 174, WATERLINE = 105;  /* rows in bg_far.png / bg_lake.png */
    draw_layer(cv, far, (int)(cx * fx), SUN_Y - 100 + (int)(below * 0.1f), LAYER_EXTEND, 0, 0);
    draw_layer(cv, lake, (int)(cx * 0.35f) + 140, WATERLINE - 212 + (int)(below * 0.3f),
               LAYER_WRAP | LAYER_ALPHA | LAYER_EXTEND, 0, 0);
}

/* sky, drifting clouds, then the sea scene. The sea's wave line is pinned to
 * the level's water surface, so the water in the gaps looks into the sea. */
static void render_island_back(Game *g, Canvas *cv, int cx, int cy, float t)
{
    int levw = g->st.lv.w * TS;
    const Image *far = &g->st.far, *sea = &g->st.mid[0], *clouds = &g->st.mid[1];
    float fx = levw > VIEW_W ? (float)(far->w - VIEW_W) / (levw - VIEW_W) : 0;
    if (fx > 0.5f) fx = 0.5f;
    const int SKY_HORIZON = 209;                        /* row of bg_far.png on the sea horizon */
    const int SEA_HORIZON = 139, SEA_WAVE = 196;        /* rows in bg_sea.png */
    const int CLOUD_BASE = 149;                         /* lowest cloud row in bg_clouds.png */
    int wave = g->water_y - cy, horizon = wave - (SEA_WAVE - SEA_HORIZON);
    gfx_fill(cv, 0, 0, VIEW_W, VIEW_H, far->px[0] & 0xffffff);
    draw_layer(cv, far, (int)(cx * fx), SKY_HORIZON - horizon, LAYER_EXTEND, 0, 0);
    draw_layer(cv, clouds, (int)(cx * 0.12f + t * 3.0f), CLOUD_BASE - (horizon - 3), LAYER_WRAP | LAYER_ALPHA, 0, 0);
    draw_layer(cv, sea, (int)(cx * 0.3f) + 120, SEA_WAVE - wave, LAYER_WRAP | LAYER_ALPHA | LAYER_EXTEND, 0, 0);
}

/* the waterfall village far off, then the lantern-lit mine cavern */
static void render_cave_back(Game *g, Canvas *cv, int cx, int below, float t)
{
    int levw = g->st.lv.w * TS;
    const Image *far = &g->st.far, *mine = &g->st.mid[0];
    float fx = levw > VIEW_W ? (float)(far->w - VIEW_W) / (levw - VIEW_W) : 0;
    if (fx > 0.5f) fx = 0.5f;
    draw_layer(cv, far, (int)(cx * fx), 22 + (int)(below * 0.1f), LAYER_EXTEND, 0, 0);
    draw_mist(cv, cx, t, 150 + (int)(below * -0.1f), 60, 0.15f, 45);
    draw_layer(cv, mine, (int)(cx * 0.4f) + 300, 26 + (int)(below * 0.3f),
               LAYER_WRAP | LAYER_ALPHA | LAYER_EXTEND, 0x0a1238, 115);
    draw_mist(cv, cx, t * 1.4f, 200 - below / 3, 50, 0.6f, 35);
}

/* big out-of-focus plants right in front of the camera: blurred, soft-edged,
 * scrolling faster than the play layer in both directions. They hang by their
 * top edge so their cut-off bases never come into view. */
static void render_lake_front(Game *g, Canvas *cv, int cx, int below)
{
    const Variants *v = g->fgblur.n ? &g->fgblur : &g->fg;
    if (!v->n) return;
    const int PERIOD = 560;
    const float PAR = 1.6f;
    float base = cx * PAR;
    int k0 = (int)floorf((base - 400) / PERIOD), k1 = (int)floorf((base + VIEW_W + 400) / PERIOD);
    for (int k = k0; k <= k1; k++) {
        uint32_t h = hash2(k, 4242);
        if (h % 5 == 0) continue;
        int x = k * PERIOD + (int)(h >> 8) % 200 - (int)base;
        const Sprite *s = pick(v, h >> 3);
        if (!s) continue;
        /* a third to a half of the plant shows at a typical camera height:
         * enough to reach its dense middle, never its base */
        int show = s->h * (33 + (int)(h >> 16) % 18) / 100;
        int y = VIEW_H - show - (int)((below + 20) * PAR);
        if (v == &g->fg) y += s->ay;
        draw_sprite(cv, s, x, y, ((h >> 20) & 1 ? SPR_FLIP : 0) | SPR_BLEND, 0, 0);
    }
}

void game_render(Game *g, Canvas *cv)
{
    int cx = (int)floorf(g->camx + 0.5f), cy = (int)floorf(g->camy + 0.5f);
    int levh = g->st.lv.h * TS;
    int below = cy - (levh - VIEW_H);       /* 0 at the bottom of the level, negative above */
    float t = g->time;
    bool moon = g->st.def->theme == TH_MOON;

    Theme th = g->st.def->theme;
    if (moon) render_moon_back(g, cv, cx, cy, below, t);
    else if (th == TH_ISLAND) render_island_back(g, cv, cx, cy, t);
    else if (th == TH_CAVE) render_cave_back(g, cv, cx, below, t);
    else render_lake_back(g, cv, cx, below);

    for (int i = 0; i < g->ndeco; i++) {
        const Deco *d = &g->deco[i];
        if (d->back)
            draw_sprite(cv, d->s, d->x - cx, d->y - cy, d->flags, moon ? 0x141a44 : 0, moon ? 110 : 0);
    }
    draw_spans(g, cv, cx, cy);

    if (moon) draw_tiles_moon(g, cv, cx, cy);
    else if (th == TH_ISLAND) draw_tiles_island(g, cv, cx, cy);
    else if (th == TH_CAVE) draw_tiles_cave(g, cv, cx, cy);
    else draw_tiles_lake(g, cv, cx, cy);
    for (int i = 0; i < g->nplat; i++) {
        const PlatRun *pr = &g->plat[i];
        if (!pr->s) { draw_plat_island(g, cv, pr, cx, cy); continue; }
        draw_sprite(cv, pr->s, pr->tx * TS + pr->len * TS / 2 - cx, pr->ty * TS - 3 - cy, 0, 0, 0);
    }
    for (int i = 0; i < g->ndeco; i++) {
        const Deco *d = &g->deco[i];
        if (!d->back) draw_sprite(cv, d->s, d->x - cx, d->y - cy, d->flags, 0, 0);
    }
    if (g->has_goal) {
        int gx = g->goal_tx * TS + TS / 2 - cx, gy = g->goal_ty * TS + TS - cy;
        bool ready = g->coins_got == g->ncoins;
        uint32_t warm = ready ? 0xffc040 : 0xff9a40;
        /* a halo, then slow light rays fanning up behind the arch: blended toward
         * gold rather than added, so they show on the bright island sky too */
        gfx_glow(cv, gx, gy - 18, ready ? 58 : 46, warm, (int)((ready ? 110 : 80) + 30 * sinf(t * 2.2f)));
        int ox = gx, oy = gy - 18, len = ready ? 92 : 76;
        for (int y = oy - len; y < oy; y++)
            for (int x = ox - len; x <= ox + len; x++) {
                if ((unsigned)x >= (unsigned)cv->w || (unsigned)y >= (unsigned)cv->h) continue;
                float dx = (float)(x - ox), dy = (float)(y - oy), d = sqrtf(dx * dx + dy * dy);
                if (d > len || d < 10) continue;
                float ang = atan2f(dx, -dy) + sinf(t * 0.5f) * 0.12f;     /* 0 straight up */
                float w = cosf(ang * 4.0f * 1.25f);                      /* five beams over +-72 deg */
                if (fabsf(ang) > 1.26f || w < 0.4f) continue;
                float a = sqrtf((w - 0.4f) / 0.6f) * (1.0f - d / len) * (0.7f + 0.3f * sinf(t * 1.9f + ang * 3));
                uint32_t *px = &cv->px[y * cv->w + x];
                *px = gfx_mix(*px, ready ? 0xfff0a0 : 0xffe0b0, (int)(a * (ready ? 200 : 150)));
            }
        draw_sprite(cv, g->goal, gx, gy, 0, 0, 0);
        gfx_glow(cv, gx, gy - 16, 20, 0xffb050, (int)(130 + 40 * sinf(t * 3)));    /* the window */
        for (int k = 0; k < 2; k++) {       /* two sparkles circling the arch */
            float a = t * 1.6f + k * 3.1416f;
            draw_sprite(cv, g->sparkle, gx + (int)(cosf(a) * 29), gy - 20 + (int)(sinf(a) * 12),
                        ((int)(t * 8) + k) & 1 ? SPR_FLIP : 0, 0, 0);
        }
        /* a beacon star over the keystone, pulsing (bigger once every coin is in) */
        int sx = gx, sy = gy - 46 + (int)lroundf(sinf(t * 2.0f) * 2);
        int arm = (ready ? 6 : 4) + (sinf(t * 5.0f) > 0.3f ? 1 : 0);
        gfx_glow(cv, sx, sy, 14, 0xffd860, 120);
        for (int k = -arm; k <= arm; k++) {
            uint32_t c = abs(k) <= 1 ? 0xffffff : abs(k) < arm - 1 ? 0xfff0a0 : 0xffc040;
            gfx_put(cv, sx + k, sy, c);
            gfx_put(cv, sx, sy + k, c);
        }
        gfx_put(cv, sx - 1, sy - 1, 0xfff0a0); gfx_put(cv, sx + 1, sy - 1, 0xfff0a0);
        gfx_put(cv, sx - 1, sy + 1, 0xfff0a0); gfx_put(cv, sx + 1, sy + 1, 0xfff0a0);
    }

    for (int i = 0; i < g->ncoins; i++) {
        const Coin *c = &g->coins[i];
        if (c->taken) continue;
        static const int SEQ[6] = { 0, 1, 2, 3, 2, 1 };
        int f = SEQ[(int)(t * 10 + c->x * 0.05f) % 6];
        draw_sprite(cv, g->coin[f], (int)c->x - cx, (int)c->y - cy, f >= 2 && ((int)(t * 5) & 1) ? SPR_FLIP : 0, 0, 0);
    }
    draw_enemies(g, cv, cx, cy);
    draw_player(g, cv, cx, cy);
    if (!moon) draw_water(g, cv, cx, cy, t);
    if (th == TH_ISLAND) draw_dock_splashes(g, cv, cx, cy, t);

    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *q = &g->part[i];
        if (q->life <= 0) continue;
        int f = (int)(q->t / q->life * q->nf);
        if (f >= q->nf) f = q->nf - 1;
        draw_sprite(cv, q->fr[f], (int)q->x - cx, (int)q->y - cy, q->flags, 0, 0);
    }
    for (int i = 0; i < MAX_SHOTS; i++) {   /* water squirts: a wobbling blob tilted along its arc */
        const Shot *sh = &g->shot[i];
        if (!sh->live) continue;
        if (g->squirt[0][2]) {
            float deg = atanf(sh->vy / fmaxf(fabsf(sh->vx), 1.0f)) * 57.2958f;
            int k = (int)lroundf(deg / 25.0f) + 2;
            k = k < 0 ? 0 : k > 4 ? 4 : k;
            const Sprite *s = g->squirt[(int)(sh->t * 12) % 3][k];
            if (s) {
                gfx_glow(cv, (int)sh->x - cx, (int)sh->y - cy, 12, 0x40c0ff, 70);
                draw_sprite(cv, s, (int)sh->x - cx, (int)sh->y - cy, sh->vx < 0 ? SPR_FLIP : 0, 0, 0);
                continue;
            }
        }
        static const char *const DROP[5] = { ".o.", "oho", "ooo", "ooo", ".o." };
        int x0 = (int)sh->x - cx - 1, y0 = (int)sh->y - cy - 2;
        gfx_glow(cv, x0 + 1, y0 + 2, 7, 0x2050a0, 60);
        for (int j = 0; j < 5; j++)
            for (int k = 0; k < 3; k++) {
                char c = DROP[j][k];
                if (c != '.') gfx_put(cv, x0 + k, y0 + j, c == 'h' ? 0xf0fcff : j < 2 ? 0x8ad8ff : 0x3a90e0);
            }
    }
    for (int i = 0; i < MAX_MOTES; i++) {   /* the exit's motes: a twinkling point of light */
        const Mote *m = &g->mote[i];
        if (m->life <= 0) continue;
        float u = m->t / m->life, fade = u < 0.2f ? u / 0.2f : 1.0f - (u - 0.2f) / 0.8f;
        int x = (int)m->x - cx, y = (int)m->y - cy;
        if (fade < 0.25f && ((int)(m->t * 20) & 1)) continue;       /* blink out */
        gfx_glow(cv, x, y, 6, m->c, (int)(110 * fade));
        uint32_t c = m->c, edge = gfx_mix(m->c, 0xb06020, 90);
        gfx_put(cv, x, y, 0xffffff);
        gfx_put(cv, x - 1, y, c); gfx_put(cv, x + 1, y, c);
        gfx_put(cv, x, y - 1, c); gfx_put(cv, x, y + 1, c);
        if (fade > 0.55f && sinf(m->t * 9.0f + m->ph) > 0.2f) {     /* a twinkle */
            gfx_put(cv, x - 2, y, edge); gfx_put(cv, x + 2, y, edge);
            gfx_put(cv, x, y - 2, edge); gfx_put(cv, x, y + 2, edge);
        }
    }
    for (int i = 0; i < MAX_POPS; i++) {    /* "-0.5S" rising from a defeated enemy */
        const Pop *q = &g->pop[i];
        if (q->t <= 0) continue;
        int w = gfx_text_width(q->txt, 1), x = (int)q->x - cx - w / 2, y = (int)q->y - cy;
        gfx_text(cv, x + 1, y + 1, q->txt, 0x10102a);
        gfx_text(cv, x, y, q->txt, q->t > 0.3f || ((int)(q->t * 20) & 1) ? 0x9af06a : 0x4a8a3a);
    }

    /* warm light from torches and lanterns, after sprites so it washes over them */
    draw_glows(g, cv, cx, cy, t);

    if (moon)   /* dark foreground foliage along the bottom edge */
        draw_layer(cv, &g->st.mid[0], (int)(cx * 1.25f) + 300, 182 - (VIEW_H - 34) + 18 + (int)(below * 1.25f),
                   LAYER_WRAP | LAYER_ALPHA, 0x03040c, 225);
    else
        render_lake_front(g, cv, cx, below);

    /* HUD (gone under the results screen) */
    if (g->state == ST_CLEAR && g->state_t > 0.7f) return;
    for (int i = 0; i < START_HEARTS; i++) draw_heart(cv, 8 + i * 22, 8, i < g->hearts);
    char buf[48];
    snprintf(buf, sizeof buf, "%d/%d", g->coins_got, g->ncoins);
    int tw = gfx_text_width(buf, 2);
    bool all = g->coins_got == g->ncoins;
    draw_sprite(cv, g->coin[0], VIEW_W - tw - 22, 16, 0, 0, 0);
    gfx_text_scaled(cv, VIEW_W - tw - 8 + 1, 12 + 1, 2, buf, 0x10102a);
    gfx_text_scaled(cv, VIEW_W - tw - 8, 12, 2, buf, all ? 0xffd860 : 0xf4f0e0);
    /* the clock, already less the kill bonus; it flashes green as a bonus lands */
    game_fmt_time(buf, sizeof buf, game_final_time(g->level_time, g->bonus));
    tw = gfx_text_width(buf, 2);
    uint32_t cc = g->bonus_t > 0 ? 0x9af06a : !g->clock_on ? 0xa8a0d0 : 0xf4f0e0;
    gfx_text_scaled(cv, (VIEW_W - tw) / 2 + 1, 12 + 1, 2, buf, 0x10102a);
    gfx_text_scaled(cv, (VIEW_W - tw) / 2, 12 - (g->bonus_t > 0.45f ? 1 : 0), 2, buf, cc);

    if (g->state == ST_PLAY && g->state_t < 2.5f) {    /* level title card */
        snprintf(buf, sizeof buf, "LEVEL %d", g->level + 1);
        int w1 = gfx_text_width(buf, 2), w2 = gfx_text_width(g->st.def->title, 3);
        int y = 70;
        gfx_text_scaled(cv, (VIEW_W - w1) / 2 + 1, y + 1, 2, buf, 0x10102a);
        gfx_text_scaled(cv, (VIEW_W - w1) / 2, y, 2, buf, 0xe8ecff);
        gfx_text_scaled(cv, (VIEW_W - w2) / 2 + 2, y + 16 + 2, 3, g->st.def->title, 0x10102a);
        gfx_text_scaled(cv, (VIEW_W - w2) / 2, y + 16, 3, g->st.def->title, 0xffd860);
    }
    if (g->state == ST_DEAD && g->state_t > 0.3f) {
        const char *m = "OUCH!";
        int w = gfx_text_width(m, 4);
        gfx_text_scaled(cv, (VIEW_W - w) / 2 + 2, VIEW_H / 2 - 12 + 2, 4, m, 0x10102a);
        gfx_text_scaled(cv, (VIEW_W - w) / 2, VIEW_H / 2 - 12, 4, m, 0xff6070);
    }
}
