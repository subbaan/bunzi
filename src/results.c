/* The level results screen and the per-level Top 10 times.
 *
 * Only perfect clears (every coin) are ranked, by the clock less the kill
 * bonus, in hundredths. Each table belongs to one level layout (its hash), so
 * editing a level starts it a fresh table and the old one stays in the file.
 * The file is rewritten through a temporary and a rename, so a crash mid-save
 * leaves the previous scores intact. */
#include "results.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "game.h"
#include "sound.h"

#define TOP 10
#define MAX_ENTRIES 2048

typedef struct { uint64_t hash; int level, cs; char ini[4]; } Entry;

struct Results {
    Entry e[MAX_ENTRIES];
    int ne;
    char last_ini[4];
    char dir[1024], path[1100];
    bool readonly;              /* asked for, or the file could not be read safely */

    RunStats rs;
    int final_cs, prev_best, rank, new_idx;     /* -1: none */
    bool perfect, record, save_failed;
    int table[TOP], nt;
    bool typing;
    int cur;
    ResultsAction opt[3];
    int nopt, sel;
    float t;
};

/* ------------------------------------------------------------ the file */

/* sorting a level's entries: by time, then file order (the earlier time keeps a tie) */
static const Entry *g_sort;
static int cmp_idx(const void *a, const void *b)
{
    int i = *(const int *)a, j = *(const int *)b;
    if (g_sort[i].cs != g_sort[j].cs) return g_sort[i].cs < g_sort[j].cs ? -1 : 1;
    return i - j;
}

static void load(Results *r)
{
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') snprintf(r->dir, sizeof r->dir, "%s/bunzi", xdg);
    else if (home && home[0]) snprintf(r->dir, sizeof r->dir, "%s/.local/share/bunzi", home);
    else { r->readonly = true; return; }
    snprintf(r->path, sizeof r->path, "%s/scores.txt", r->dir);
    FILE *f = fopen(r->path, "r");
    if (!f) {
        if (errno != ENOENT) r->readonly = true;    /* there, but unreadable: leave it be */
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        Entry e = { 0 };
        char ini[8];
        if (sscanf(line, "initials %3s", ini) == 1) {
            snprintf(r->last_ini, sizeof r->last_ini, "%.3s", ini);
        } else if (sscanf(line, "%" SCNx64 " %d %d %3s", &e.hash, &e.level, &e.cs, ini) == 4) {
            if (r->ne == MAX_ENTRIES) { r->readonly = true; break; }   /* never drop records */
            snprintf(e.ini, sizeof e.ini, "%.3s", ini);
            r->e[r->ne++] = e;
        }
    }
    fclose(f);
}

static int save(Results *r)
{
    if (r->readonly) return -1;
    /* the data directory, one level at a time ($HOME/.local/share may not exist yet) */
    char p[1024];
    snprintf(p, sizeof p, "%s", r->dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    if (mkdir(p, 0755) && errno != EEXIST) return -1;

    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", r->path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# bunzi scores: level-hash level centiseconds initials (perfect clears only)\n");
    if (r->last_ini[0]) fprintf(f, "initials %s\n", r->last_ini);
    for (int i = 0; i < r->ne; i++)
        fprintf(f, "%016" PRIx64 " %d %d %s\n", r->e[i].hash, r->e[i].level, r->e[i].cs, r->e[i].ini);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, r->path)) { unlink(tmp); return -1; }
    return 0;
}

/* this level's entries, best first */
static void build_table(Results *r)
{
    static int idx[MAX_ENTRIES];
    int n = 0;
    for (int i = 0; i < r->ne; i++)
        if (r->e[i].hash == r->rs.level_hash) idx[n++] = i;
    g_sort = r->e;
    qsort(idx, (size_t)n, sizeof *idx, cmp_idx);
    r->nt = n < TOP ? n : TOP;
    memcpy(r->table, idx, sizeof(int) * (size_t)r->nt);
}

/* keep the best TOP of this level's table; returns where entry `track` went */
static int prune(Results *r, int track)
{
    build_table(r);
    static bool keep[MAX_ENTRIES];
    memset(keep, 0, sizeof keep);
    for (int k = 0; k < r->nt; k++) keep[r->table[k]] = true;
    int n = 0, moved = -1;
    for (int i = 0; i < r->ne; i++)
        if (r->e[i].hash != r->rs.level_hash || keep[i]) {
            if (i == track) moved = n;
            r->e[n++] = r->e[i];
        }
    r->ne = n;
    build_table(r);
    return moved;
}

/* ------------------------------------------------------------- public */

Results *results_create(bool readonly)
{
    Results *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->readonly = readonly;
    load(r);
    if (!r->last_ini[0]) snprintf(r->last_ini, sizeof r->last_ini, "BUN");
    return r;
}

void results_destroy(Results *r) { free(r); }

bool results_typing(const Results *r) { return r->typing; }

void results_begin(Results *r, const RunStats *rs)
{
    r->rs = *rs;
    r->t = 0;
    r->typing = r->record = r->save_failed = false;
    r->rank = r->new_idx = r->prev_best = -1;
    r->cur = 0;
    float fin = game_final_time(rs->time, rs->bonus);
    r->final_cs = (int)(fin * 100.0f + 0.5f);
    r->perfect = rs->coins == rs->coins_total;
    build_table(r);
    if (r->nt) r->prev_best = r->e[r->table[0]].cs;
    if (r->perfect && !rs->practice) {
        int rank = 0;
        while (rank < r->nt && r->e[r->table[rank]].cs <= r->final_cs) rank++;
        if (rank < TOP && r->ne < MAX_ENTRIES) {
            Entry e = { rs->level_hash, rs->level + 1, r->final_cs, "" };
            memcpy(e.ini, r->last_ini, sizeof e.ini);
            r->new_idx = r->ne;
            r->e[r->ne++] = e;
            build_table(r);
            r->rank = rank;
            r->record = rank == 0;
            r->typing = true;
            snd_play(r->record ? SFX_RECORD : SFX_SELECT, 1.0f, 1.0f, 0);
        }
    }
    r->nopt = 0;
    r->opt[r->nopt++] = RA_RETRY;
    if (rs->has_next) r->opt[r->nopt++] = RA_NEXT;
    if (rs->has_title) r->opt[r->nopt++] = RA_TITLE;
    r->sel = r->perfect && rs->has_next ? 1 : 0;
}

static void finish_entry(Results *r)
{
    r->typing = false;
    char *ini = r->e[r->new_idx].ini;
    for (int k = 0; k < 3; k++) if (ini[k] < 'A' || ini[k] > 'Z') ini[k] = 'A';
    memcpy(r->last_ini, ini, sizeof r->last_ini);
    r->new_idx = prune(r, r->new_idx);
    r->save_failed = save(r) != 0 && !r->readonly;
    snd_play(SFX_SELECT, 1.0f, 1.0f, 0);
}

ResultsAction results_update(Results *r, const TitleInput *in, char typed, float dt)
{
    r->t += dt;
    if (!in || r->t < 0.5f) return RA_NONE;
    if (r->typing) {
        char *ini = r->e[r->new_idx].ini;
        if (typed >= 'A' && typed <= 'Z') {
            ini[r->cur] = typed;
            if (r->cur < 2) r->cur++;
            snd_play(SFX_MENU, 1.0f, 1.0f, 0);
        } else if (typed == '\b' || in->back || in->left) {
            if (r->cur > 0) { r->cur--; snd_play(SFX_MENU, 1.0f, 0.9f, 0); }
        } else if (in->right && r->cur < 2) {
            r->cur++;
            snd_play(SFX_MENU, 1.0f, 1.0f, 0);
        } else if (in->up || in->down) {
            int c = ini[r->cur] - 'A' + (in->up ? 1 : 25);
            ini[r->cur] = (char)('A' + c % 26);
            snd_play(SFX_MENU, 1.0f, 1.0f, 0);
        } else if (typed == '\n' || (in->confirm && r->cur == 2)) {
            finish_entry(r);
        } else if (in->confirm) {
            r->cur++;
            snd_play(SFX_MENU, 1.0f, 1.0f, 0);
        }
        return RA_NONE;
    }
    if (in->left || in->up) { r->sel = (r->sel + r->nopt - 1) % r->nopt; snd_play(SFX_MENU, 1.0f, 1.0f, 0); }
    if (in->right || in->down) { r->sel = (r->sel + 1) % r->nopt; snd_play(SFX_MENU, 1.0f, 1.0f, 0); }
    if (in->confirm || typed == '\n') { snd_play(SFX_SELECT, 1.0f, 1.0f, 0); return r->opt[r->sel]; }
    if (in->back && r->rs.has_title) return RA_TITLE;
    return RA_NONE;
}

/* ------------------------------------------------------------- drawing */

static const uint32_t INK = 0x10102a, WHITE = 0xf4f0e0, GOLD = 0xffd860, DIM = 0xa8a0d0,
                      GREEN = 0x9af06a, PINK = 0xff8aa0;

static void txt(Canvas *cv, int x, int y, int s, const char *m, uint32_t c)
{
    int o = s >= 3 ? 2 : 1;     /* drop shadow */
    gfx_text_scaled(cv, x + o, y + o, s, m, INK);
    gfx_text_scaled(cv, x, y, s, m, c);
}

static void ctxt(Canvas *cv, int y, int s, const char *m, uint32_t c)
{
    txt(cv, (cv->w - gfx_text_width(m, s)) / 2, y, s, m, c);
}

static void rtxt(Canvas *cv, int xr, int y, int s, const char *m, uint32_t c)
{
    txt(cv, xr - gfx_text_width(m, s), y, s, m, c);
}

static const char *ordinal(int n)
{
    static char b[16];
    snprintf(b, sizeof b, "%d%s", n, n == 1 ? "ST" : n == 2 ? "ND" : n == 3 ? "RD" : "TH");
    return b;
}

void results_render(const Results *r, Canvas *cv)
{
    float e = fminf(1.0f, r->t / 0.35f);
    e = 1.0f - (1.0f - e) * (1.0f - e);
    gfx_tint(cv, 0, 0, cv->w, cv->h, 0x05061a, (int)(185 * e));
    if (r->t < 0.12f) return;
    int dy = (int)(18 * (1.0f - e));
    bool blink = (int)(r->t * 3) & 1;
    char b[64], tm[24];

    snprintf(b, sizeof b, "LEVEL %d  %s", r->rs.level + 1, r->rs.title ? r->rs.title : "");
    ctxt(cv, 12 + dy, 1, b, DIM);
    ctxt(cv, 24 + dy, 3, r->perfect ? "PERFECT CLEAR!" : "LEVEL CLEAR", r->perfect ? GOLD : WHITE);
    snprintf(b, sizeof b, "COINS %d/%d", r->rs.coins, r->rs.coins_total);
    ctxt(cv, 48 + dy, 2, b, r->perfect ? GOLD : WHITE);

    /* the breakdown, left */
    int lx = 36, lr = 220, y = 80 + dy;
    txt(cv, lx, y, 2, "TIME", DIM);
    game_fmt_time(tm, sizeof tm, r->rs.time);
    rtxt(cv, lr, y, 2, tm, WHITE);
    y += 18;
    snprintf(b, sizeof b, "BONUS (%d)", r->rs.kills);
    txt(cv, lx, y, 2, b, DIM);
    tm[0] = '-';
    game_fmt_time(tm + 1, sizeof tm - 1, r->rs.bonus);
    rtxt(cv, lr, y, 2, tm, GREEN);
    y += 14;
    gfx_fill(cv, lx, y, lr - lx, 1, 0x50487a);
    y += 8;
    txt(cv, lx, y + 5, 2, "FINAL", DIM);
    game_fmt_time(tm, sizeof tm, r->final_cs / 100.0f);
    rtxt(cv, lr, y, 3, tm, r->perfect ? GOLD : WHITE);
    y += 28;

    /* how it went */
    const char *msg = NULL;
    uint32_t mc = WHITE;
    if (r->rs.practice) { msg = "PRACTICE RUN - NOT RECORDED"; mc = DIM; }
    else if (!r->perfect) { msg = "COLLECT EVERY COIN TO SET A TIME"; mc = DIM; }
    else if (r->record) { msg = r->prev_best >= 0 ? "NEW RECORD!" : "FIRST RECORD!"; mc = blink ? GOLD : WHITE; }
    else if (r->rank >= 0) { snprintf(b, sizeof b, "YOU PLACED %s", ordinal(r->rank + 1)); msg = b; mc = GOLD; }
    else if (r->prev_best >= 0) {
        game_fmt_time(tm, sizeof tm, (r->final_cs - r->prev_best) / 100.0f);
        snprintf(b, sizeof b, "+%s OFF THE RECORD", tm);
        msg = b;
        mc = DIM;
    }
    if (msg) txt(cv, (lx + lr - gfx_text_width(msg, msg == b || r->record ? 2 : 1)) / 2, y,
                 msg == b || r->record ? 2 : 1, msg, mc);
    if (r->record && r->prev_best >= 0) {
        game_fmt_time(tm, sizeof tm, (r->prev_best - r->final_cs) / 100.0f);
        snprintf(b, sizeof b, "%s FASTER", tm);
        txt(cv, (lx + lr - gfx_text_width(b, 1)) / 2, y + 16, 1, b, GREEN);
    }
    if (r->save_failed) ctxt(cv, cv->h - 12, 1, "COULD NOT SAVE THE SCORES", PINK);

    /* the table, right */
    int tx = 262, tr = 446, ty = 74 + dy;
    txt(cv, tx, ty, 2, "TOP TIMES", WHITE);
    rtxt(cv, tr, ty + 3, 1, "100% CLEARS", DIM);
    ty += 18;
    for (int k = 0; k < TOP; k++, ty += 12) {
        bool mine = k < r->nt && r->table[k] == r->new_idx && r->new_idx >= 0;
        uint32_t c = mine ? GOLD : k == 0 && k < r->nt ? WHITE : 0xd0c8e8;
        snprintf(b, sizeof b, "%02d", k + 1);
        txt(cv, tx, ty, 2, b, mine ? GOLD : DIM);
        if (k >= r->nt) {
            txt(cv, tx + 32, ty, 2, "---", 0x50487a);
            rtxt(cv, tr, ty, 2, "-:--.--", 0x50487a);
            continue;
        }
        const Entry *en = &r->e[r->table[k]];
        for (int j = 0; j < 3; j++) {
            char ch[2] = { en->ini[j] ? en->ini[j] : ' ', 0 };
            bool at = mine && r->typing && j == r->cur;
            if (at) gfx_fill(cv, tx + 32 + j * 8 - 1, ty + 11, 7, 1, blink ? GOLD : 0x806a30);
            txt(cv, tx + 32 + j * 8, ty, 2, ch, at && blink ? WHITE : c);
        }
        game_fmt_time(tm, sizeof tm, en->cs / 100.0f);
        rtxt(cv, tr, ty, 2, tm, c);
        if (mine) gfx_tint(cv, tx - 4, ty - 2, tr - tx + 8, 14, 0xffd860, 28);
    }

    /* the bottom line */
    int by = 236 + dy;
    if (r->typing) {
        ctxt(cv, by - 4, 2, "ENTER YOUR INITIALS", blink ? GOLD : WHITE);
        ctxt(cv, by + 14, 1, "TYPE OR UP/DOWN  -  ENTER WHEN DONE", DIM);
        return;
    }
    static const char *const NAME[] = { [RA_RETRY] = "RETRY", [RA_NEXT] = "NEXT LEVEL", [RA_TITLE] = "TITLE" };
    int w = 0, gap = 36;
    for (int k = 0; k < r->nopt; k++) w += gfx_text_width(NAME[r->opt[k]], 2) + (k ? gap : 0);
    int x = (cv->w - w) / 2;
    for (int k = 0; k < r->nopt; k++) {
        const char *m = NAME[r->opt[k]];
        int mw = gfx_text_width(m, 2);
        bool s = k == r->sel;
        if (s) {
            txt(cv, x - 14, by, 2, ">", GOLD);
            txt(cv, x + mw + 6, by, 2, "<", GOLD);
        }
        txt(cv, x, by, 2, m, s ? GOLD : DIM);
        x += mw + gap;
    }
}
