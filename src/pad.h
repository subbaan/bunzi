/* Game controller input through SDL3 (no window, no video subsystem). */
#ifndef PAD_H
#define PAD_H

#include <stdbool.h>

/* The buttons that can be bound, named by position as on an Xbox pad: A is
 * the bottom face button, B the right, X the left, Y the top. The triggers
 * count as buttons past half way. */
enum {
    PB_NONE, PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_LS, PB_RS,
    PB_BACK, PB_START, PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_P1, PB_P2, PB_P3, PB_P4, PB_MISC,
    PB_COUNT
};

typedef struct {
    bool connected;
    char name[64];
    bool left, right, up, down;     /* d-pad or left stick past the deadzone (menus) */
    bool stick_left, stick_right, stick_up, stick_down;     /* the left stick alone */
    bool btn[PB_COUNT];             /* held */
    bool btn_pressed[PB_COUNT];     /* edges since last poll */
    bool start, back;
    bool a_pressed, start_pressed, back_pressed;
    bool cancel_pressed;            /* B, the right face button (menus) */
    float lx, ly;                   /* left stick, -1..1 */
} PadState;

int  pad_init(void);                /* 0 on success; harmless to run without a pad */
void pad_poll(PadState *ps);
void pad_shutdown(void);
const char *pad_error(void);

#endif
