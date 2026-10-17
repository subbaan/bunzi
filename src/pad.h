/* Game controller input through SDL3 (no window, no video subsystem). */
#ifndef PAD_H
#define PAD_H

#include <stdbool.h>

typedef struct {
    bool connected;
    char name[64];
    bool left, right, up, down;     /* d-pad or left stick past the deadzone */
    bool jump, run;
    bool start, back;
    bool jump_pressed, start_pressed, back_pressed;   /* edges since last poll */
    bool cancel_pressed;            /* B / east button edge (menus) */
    float lx, ly;                   /* left stick, -1..1 */
} PadState;

int  pad_init(void);                /* 0 on success; harmless to run without a pad */
void pad_poll(PadState *ps);
void pad_shutdown(void);
const char *pad_error(void);

#endif
