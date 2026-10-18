/* The title screen: the dusk lake scene, the Bunzi logo, fireflies,
 * the bunny hopping in, and the menu (start, level, controls, quit). */
#ifndef TITLE_H
#define TITLE_H

#include <stdbool.h>
#include <stddef.h>

#include "gfx.h"

typedef struct Title Title;

typedef struct {
    bool up, down, left, right;     /* edges */
    bool confirm, back;
} TitleInput;

typedef enum { TA_NONE, TA_START, TA_CONTROLS, TA_QUIT } TitleAction;

Title *title_create(const char *asset_dir, char *err, size_t errn);
void   title_destroy(Title *t);
/* Start the entrance again (logo drops in, the bunny hops on). While held, only
 * the scenery moves - used under the terminal transition. */
void   title_reset(Title *t, bool hold);
void   title_hold(Title *t, bool hold);
/* only the scenery (no logo, menu or bunny): behind the controls screen */
void   title_backdrop(Title *t, bool on);
/* level: the menu's level choice (0-based), read and written */
TitleAction title_update(Title *t, const TitleInput *in, float dt, int *level);
void   title_render(Title *t, Canvas *cv);

#endif
