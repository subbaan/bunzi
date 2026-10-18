/* Keyboard and gamepad bindings for the game's actions, kept in
 * $XDG_CONFIG_HOME/bunzi/controls.txt (~/.config/bunzi), and the screen on
 * the title that changes them. The left stick always moves, and the menus
 * keep their fixed buttons. */
#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdbool.h>
#include <stddef.h>

#include "gfx.h"

typedef enum { CT_LEFT, CT_RIGHT, CT_DOWN, CT_JUMP, CT_RUN, CT_RESTART, CT_COUNT } Control;
#define CT_KEYS 4               /* keys (or buttons) per action */

void ctl_load(void);            /* the defaults, then the file if there is one */
/* keys: a K_MAX array such as g_input.down or g_input.pressed */
bool ctl_any(const bool *keys, Control c);
bool ctl_has(int key, Control c);
bool ctl_bound(int key);        /* bound to any action */
const char *ctl_key_name(int key);
/* the same for the gamepad; btn: a PB_COUNT array such as PadState.btn */
bool ctl_pad_any(const bool *btn, Control c);
bool ctl_pad_bound(int b);
const char *ctl_pad_name(int b);

typedef struct CtlScreen CtlScreen;

typedef struct {
    bool up, down, left, right;     /* edges */
    bool confirm, clear, back;
    bool esc;                       /* the Esc key alone: cancels waiting for a button */
    bool swap;                      /* keyboard <-> gamepad */
    bool pad_on;                    /* a gamepad is connected */
} CtlNav;

CtlScreen *ctl_screen_create(const char *asset_dir, char *err, size_t errn);
void ctl_screen_destroy(CtlScreen *s);
void ctl_screen_open(CtlScreen *s, bool pad);   /* pad: open on the gamepad's page */
/* pressed/down: the keyboard's edges and held keys (K_MAX arrays); pad_pressed/
 * pad_down: the gamepad's (PB_COUNT arrays, NULL without one). nav NULL:
 * animation only. Returns true when the screen is closed. */
bool ctl_screen_update(CtlScreen *s, const CtlNav *nav, const bool *pressed, const bool *down,
                       const bool *pad_pressed, const bool *pad_down, float dt);
void ctl_screen_render(const CtlScreen *s, Canvas *cv);
/* frame dumps: put the cursor on a row (0-based, -1 the page tabs, CT_COUNT
 * the buttons) */
void ctl_screen_select(CtlScreen *s, int row, bool pad, bool listening);

#endif
