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
    if (!pad) return;

    ps->connected = true;
    const char *nm = SDL_GetGamepadName(pad);
    snprintf(ps->name, sizeof ps->name, "%s", nm ? nm : "gamepad");

    ps->lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    ps->ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    float rt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;

    ps->left  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)  || ps->lx < -STICK_DEADZONE;
    ps->right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ps->lx >  STICK_DEADZONE;
    /* vertical stick only counts when it points mostly up or down, so running
     * with the thumb angled a little low doesn't trigger down+jump drop-through */
    bool vert = fabsf(ps->ly) >= fabsf(ps->lx);
    ps->up    = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP)    || (vert && ps->ly < -STICK_DEADZONE);
    ps->down  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)  || (vert && ps->ly >  STICK_DEADZONE);

    ps->jump  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    ps->run   = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST) ||
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || rt > TRIGGER_ON;
    ps->start = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    ps->back  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);

    ps->jump_pressed  = edge[SDL_GAMEPAD_BUTTON_SOUTH];
    ps->start_pressed = edge[SDL_GAMEPAD_BUTTON_START];
    ps->back_pressed  = edge[SDL_GAMEPAD_BUTTON_BACK];
    ps->cancel_pressed = edge[SDL_GAMEPAD_BUTTON_EAST];
}

void pad_shutdown(void)
{
    if (pad) SDL_CloseGamepad(pad);
    pad = NULL;
    if (sdl_ok) SDL_Quit();
    sdl_ok = false;
}
