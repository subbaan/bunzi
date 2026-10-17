/* Sound effects, ambience and music through SDL3 audio, with a small mixer. */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>

typedef enum {
    SFX_JUMP, SFX_LAND, SFX_COIN, SFX_STOMP, SFX_POOF, SFX_HURT, SFX_DIE,
    SFX_CLEAR, SFX_SKID, SFX_STEP, SFX_RESPAWN, SFX_GHOST, SFX_MENU, SFX_SELECT,
    SFX_SPLASH, SFX_RECORD,
    SFX_COUNT
} Sfx;

int  snd_init(const char *asset_dir);       /* 0 ok; the game runs silently otherwise */
void snd_play(Sfx id, float vol, float pitch, float pan);   /* pan -1..1 */
void snd_toggle_mute(void);
typedef enum { MUS_GAME, MUS_TITLE, MUS_COUNT } Music;
void snd_music(Music m);                    /* crossfade to it; the title tune starts from the top */
bool snd_muted(void);
void snd_shutdown(void);
const char *snd_status(void);

#endif
