#include "pad.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define STICK_DEADZONE 0.3f
#define TRIGGER_ON     0.5f

static bool sdl_ok;
static SDL_Gamepad *pad;
static bool edge[SDL_GAMEPAD_BUTTON_COUNT];
static bool trig_was[2];

static const SDL_GamepadButton SDL_OF[PB_COUNT] = {
    [PB_A] = SDL_GAMEPAD_BUTTON_SOUTH, [PB_B] = SDL_GAMEPAD_BUTTON_EAST,
    [PB_X] = SDL_GAMEPAD_BUTTON_WEST, [PB_Y] = SDL_GAMEPAD_BUTTON_NORTH,
    [PB_LB] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, [PB_RB] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    [PB_LS] = SDL_GAMEPAD_BUTTON_LEFT_STICK, [PB_RS] = SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    [PB_BACK] = SDL_GAMEPAD_BUTTON_BACK, [PB_START] = SDL_GAMEPAD_BUTTON_START,
    [PB_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP, [PB_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    [PB_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT, [PB_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    [PB_P1] = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, [PB_P2] = SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    [PB_P3] = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, [PB_P4] = SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    [PB_MISC] = SDL_GAMEPAD_BUTTON_MISC1,
};
static char err[128];

/* SDL must never write to the terminal we are drawing on. */
static void quiet_log(void *ud, int cat, SDL_LogPriority pri, const char *msg)
{
    (void)ud; (void)cat; (void)pri; (void)msg;
}

static void open_first(void)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
}

int pad_init(void)
{
    SDL_SetLogOutputFunction(quiet_log, NULL);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* there is no window to have focus, so accept input regardless */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        snprintf(err, sizeof err, "%s", SDL_GetError());
        return -1;
    }
    sdl_ok = true;
    open_first();
    return 0;
}

const char *pad_error(void) { return err; }

void pad_poll(PadState *ps)
{
    memset(ps, 0, sizeof *ps);
    if (!sdl_ok) return;

    memset(edge, 0, sizeof edge);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                SDL_CloseGamepad(pad);
                pad = NULL;
                open_first();
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (pad && e.gbutton.which == SDL_GetGamepadID(pad) &&
                e.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT)
                edge[e.gbutton.button] = true;
            break;
        default:
            break;
        }
    }
    if (!pad) { trig_was[0] = trig_was[1] = false; return; }

    ps->connected = true;
    const char *nm = SDL_GetGamepadName(pad);
    snprintf(ps->name, sizeof ps->name, "%s", nm ? nm : "gamepad");

    ps->lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    ps->ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    float lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
    float rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;

    for (int b = 1; b < PB_COUNT; b++) {
        if (b == PB_LT || b == PB_RT) continue;
        ps->btn[b] = SDL_GetGamepadButton(pad, SDL_OF[b]);
        ps->btn_pressed[b] = edge[SDL_OF[b]];
    }
    ps->btn[PB_LT] = lt > TRIGGER_ON;
    ps->btn[PB_RT] = rt > TRIGGER_ON;
    ps->btn_pressed[PB_LT] = ps->btn[PB_LT] && !trig_was[0];
    ps->btn_pressed[PB_RT] = ps->btn[PB_RT] && !trig_was[1];
    trig_was[0] = ps->btn[PB_LT];
    trig_was[1] = ps->btn[PB_RT];

    ps->stick_left  = ps->lx < -STICK_DEADZONE;
    ps->stick_right = ps->lx >  STICK_DEADZONE;
    /* vertical stick only counts when it points mostly up or down, so running
     * with the thumb angled a little low doesn't trigger down+jump drop-through */
    bool vert = fabsf(ps->ly) >= fabsf(ps->lx);
    ps->stick_up    = vert && ps->ly < -STICK_DEADZONE;
    ps->stick_down  = vert && ps->ly >  STICK_DEADZONE;
    ps->left  = ps->btn[PB_LEFT]  || ps->stick_left;
    ps->right = ps->btn[PB_RIGHT] || ps->stick_right;
    ps->up    = ps->btn[PB_UP]    || ps->stick_up;
    ps->down  = ps->btn[PB_DOWN]  || ps->stick_down;

    ps->start = ps->btn[PB_START];
    ps->back  = ps->btn[PB_BACK];
    ps->a_pressed      = ps->btn_pressed[PB_A];
    ps->start_pressed  = ps->btn_pressed[PB_START];
    ps->back_pressed   = ps->btn_pressed[PB_BACK];
    ps->cancel_pressed = ps->btn_pressed[PB_B];
}

void pad_shutdown(void)
{
    if (pad) SDL_CloseGamepad(pad);
    pad = NULL;
    if (sdl_ok) SDL_Quit();
    sdl_ok = false;
}
