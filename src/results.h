/* The level results screen and the per-level Top 10 times (perfect clears
 * only), kept in $XDG_DATA_HOME/bunzi/scores.txt. */
#ifndef RESULTS_H
#define RESULTS_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "title.h"

typedef struct Results Results;

typedef struct {
    int level;                  /* 0-based */
    const char *title;
    uint64_t level_hash;
    int coins, coins_total, kills;
    float time, bonus;
    bool practice;              /* never recorded */
    bool has_next, has_title;   /* which menu entries to offer */
} RunStats;

typedef enum { RA_NONE, RA_RETRY, RA_NEXT, RA_TITLE } ResultsAction;

/* readonly: never write the scores file (frame dumps, benchmarks) */
Results *results_create(bool readonly);
void     results_destroy(Results *r);
void     results_begin(Results *r, const RunStats *rs);
/* true while initials are being entered: letter keys type, so the caller
 * should pass them in `typed` and keep them away from other hotkeys */
bool     results_typing(const Results *r);
/* typed: 'A'-'Z' from the keyboard, '\b' for backspace, 0 for none */
ResultsAction results_update(Results *r, const TitleInput *in, char typed, float dt);
void     results_render(const Results *r, Canvas *cv);

#endif
