/* Game state, simulation and rendering for one level. */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

#define VIEW_W 480
#define VIEW_H 270

typedef struct {
    bool left, right, up, down;
    bool jump, run;
    bool jump_pressed;      /* edge, consumed by the first sim step it is seen in */
} Actions;

typedef struct Game Game;

typedef struct {
    float x, y, vx, vy;
    bool ground;
    int hearts, coins, coins_total;
    int level;                  /* 0-based */
    const char *state, *level_title;
    bool clear;                 /* reached the exit; clear_t: seconds since */
    float clear_t;
    float time, bonus;          /* the clock (from the first move) and seconds earned by kills */
    int kills;
    bool practice;              /* a debug teleport was used: not a record run */
    uint64_t level_hash;        /* fingerprint of the level layout */
} GameInfo;

/* The level list: count, text file path, title, and which decor characters
 * hang from the cell above (for --check). */
int         game_level_count(void);
const char *game_level_path(int level, const char *asset_dir, char *buf, size_t n);
const char *game_level_title(int level);
const char *game_level_hangs(int level);

Game *game_create(const char *asset_dir, int level, char *err, size_t errn);
void  game_destroy(Game *g);
void  game_restart(Game *g);
int   game_set_level(Game *g, int level, char *err, size_t errn);   /* 0 ok, keeps the old level on failure */
void  game_update(Game *g, const Actions *a, float dt);
void  game_render(Game *g, Canvas *cv);
void  game_info(const Game *g, GameInfo *out);
void  game_teleport(Game *g, int tx);   /* debug: stand on the ground at column tx */
float game_final_time(float time, float bonus);         /* the clock less the bonus, never below 0 */
void  game_fmt_time(char *buf, size_t n, float secs);   /* M:SS.CC */

#endif
