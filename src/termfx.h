/* Terminal <-> game transition: the launching terminal's own text dissolves
 * into the game world (intro) and reassembles on quit (outro).
 *
 * No screenshot is taken. The game stays on kitty's main screen during the
 * transition and draws a full-window RGBA image *over* the real text: where the
 * image is transparent you see the actual terminal. Character positions and
 * colours come from kitty remote control (get-text/get-colors over the socket
 * in $KITTY_LISTEN_ON, matched to $KITTY_WINDOW_ID) and are kept in memory
 * only. Without it, a soft sweep with sparks is used instead. */
#ifndef TERMFX_H
#define TERMFX_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "term.h"

typedef struct {
    uint32_t ch;            /* codepoint, 0 for the right half of a wide char */
    uint32_t fg, bg;        /* resolved 0xRRGGBB */
    bool has_bg;            /* non-default background */
} TCell;

typedef struct {
    int cols, rows, cellw, cellh;
    int W, H;               /* transition image size: the cell grid in pixels */
    TCell *cell;
    bool have_text;         /* false: sweep fallback */
    int cur_r, cur_c;       /* cursor cell (where the first spark starts) */
    uint32_t fg, bg;        /* terminal default colours */
    float *when;            /* per cell: detach (intro) / arrival (outro) time */
    uint32_t *buf;          /* W*H ARGB, what gets sent */
    struct Frag *frag;
    int nfrag, capfrag;
    int s, ox, oy;          /* how the game canvas is placed in the window */
    char status[160];
} TermFx;

/* Read the terminal (call before anything draws over it). Returns 0 when a
 * transition is possible at all; fx->have_text says whether it is per-glyph. */
int  termfx_capture(TermFx *fx, const TermSize *ts);
void termfx_free(TermFx *fx);

float termfx_intro_len(const TermFx *fx);
float termfx_outro_len(const TermFx *fx);

/* Prepare the choreography; canvas is the game frame the world comes from. */
void termfx_start(TermFx *fx, bool outro, int canvas_w, int canvas_h);

/* Compose frame at time t (seconds since start) into fx->buf. */
void termfx_render(TermFx *fx, bool outro, float t, const Canvas *world);

#endif
