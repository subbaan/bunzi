/* Terminal setup, capability probing, kitty keyboard input, buffered output. */
#ifndef TERM_H
#define TERM_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Key indices: ASCII keys map to themselves (letters lowercased). */
enum {
    K_TAB   = 9,
    K_ENTER = 13,
    K_ESC   = 27,
    K_LEFT  = 128, K_RIGHT, K_UP, K_DOWN,
    K_SHIFT, K_CTRL, K_ALT,
    K_MAX   = 256
};

typedef struct {
    int cols, rows;     /* character cells */
    int xpix, ypix;     /* window size in pixels */
    int cellw, cellh;   /* cell size in pixels */
} TermSize;

typedef struct {
    bool kb_proto;      /* kitty keyboard protocol (press/repeat/release) */
    int  kb_flags;
    bool gfx;           /* kitty graphics protocol */
    bool gfx_shm;       /* graphics via POSIX shared memory */
    bool sync_output;   /* DEC mode 2026 synchronized output */
    char gfx_err[96];
    char shm_err[96];
} TermCaps;

typedef struct {
    bool down[K_MAX];
    bool pressed[K_MAX];    /* edge: went down since last clear */
    bool released[K_MAX];   /* edge: went up since last clear */
    unsigned events;
    unsigned gfx_ok, gfx_err;
    char gfx_last_err[128];
    bool quit;              /* ctrl+c */
} InputState;

extern InputState g_input;
extern volatile sig_atomic_t g_quit, g_resized;

int  term_init(void);               /* raw mode + signal handlers */
int  term_probe(TermCaps *caps, double timeout_s);
void term_enter_game(void);         /* alt screen, hide cursor, keyboard protocol */
extern const char TERM_GAME_SEQ[];  /* what term_enter_game sends */
void term_restore(void);
int  term_get_size(TermSize *ts);
void term_read_input(void);         /* non-blocking: drain and parse stdin */
void input_clear_edges(void);

void   out_bytes(const void *p, size_t n);
void   out_str(const char *s);
void   out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
size_t out_flush(void);             /* returns bytes written */

size_t b64_encode(const uint8_t *in, size_t n, char *out);  /* out: 4*ceil(n/3)+1 */

#endif
