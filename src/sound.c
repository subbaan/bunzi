#include "sound.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE    48000
#define VOICES  24
#define CHUNK   512

typedef struct {
    float *data;        /* mono float at RATE */
    int len;
    float vol;          /* from sounds.txt */
} Clip;

typedef struct {
    const Clip *clip;
    double pos, step;
    float gl, gr;
    bool loop;
} Voice;

static const char *EVENT[SFX_COUNT] = {
    "jump", "land", "coin", "stomp", "poof", "hurt", "die",
    "clear", "skid", "step", "respawn", "ghost", "menu", "select", "splash", "record",
};
static const char *MUSIC_EVENT[MUS_COUNT] = { "music", "title_music" };

/* used when sounds.txt has no line for an event */
static const struct { const char *ev, *file; float vol; } DEFAULTS[] = {
    { "menu", "menu_a.wav", 0.35f }, { "select", "select_a.wav", 0.45f },
    { "title_music", "music_title.wav", 0.55f },
    { "splash", "splash_a.wav", 0.40f }, { "record", "record_a.wav", 0.60f },
};

typedef struct { Clip clip; double pos; float gain, target; } Track;    /* looping music */
#define FADE_PER_FRAME (1.0f / (RATE * 0.8f))                           /* 0.8s crossfade */

static Clip clips[SFX_COUNT], ambience;
static Track tracks[MUS_COUNT];
static Voice voices[VOICES];
static SDL_AudioStream *stream;
static SDL_Mutex *lock;
static bool ready, muted;
static char status[160] = "sound off";

static int load_clip(Clip *c, const char *dir, const char *file, float vol)
{
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    SDL_AudioSpec spec;
    Uint8 *buf = NULL, *out = NULL;
    Uint32 len = 0;
    int outlen = 0;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return -1;
    SDL_AudioSpec dst = { SDL_AUDIO_F32, 1, RATE };
    bool ok = SDL_ConvertAudioSamples(&spec, buf, (int)len, &dst, &out, &outlen);
    SDL_free(buf);
    if (!ok) return -1;
    c->data = (float *)out;
    c->len = outlen / (int)sizeof(float);
    c->vol = vol;
    return 0;
}

static void start_voice(const Clip *c, float vol, float pitch, float pan, bool loop)
{
    if (!c->data) return;
    SDL_LockMutex(lock);
    int pick = -1;
    for (int i = 0; i < VOICES && pick < 0; i++)
        if (!voices[i].clip) pick = i;
    if (pick < 0) {         /* steal the one-shot furthest through */
        double best = -1;
        for (int i = 0; i < VOICES; i++)
            if (!voices[i].loop && voices[i].pos / voices[i].clip->len > best) {
                best = voices[i].pos / voices[i].clip->len;
                pick = i;
            }
    }
    if (pick >= 0) {
        float v = vol * c->vol;
        if (pan < -1) pan = -1;
        if (pan > 1) pan = 1;
        voices[pick] = (Voice){ c, 0, pitch > 0.05f ? pitch : 1.0f,
                                v * sqrtf(0.5f * (1 - pan)), v * sqrtf(0.5f * (1 + pan)), loop };
    }
    SDL_UnlockMutex(lock);
}

static void SDLCALL feed(void *ud, SDL_AudioStream *st, int additional, int total)
{
    (void)ud; (void)total;
    float mixbuf[CHUNK * 2];
    int frames = additional / (int)(2 * sizeof(float));
    while (frames > 0) {
        int n = frames < CHUNK ? frames : CHUNK;
        memset(mixbuf, 0, sizeof(float) * 2 * n);
        SDL_LockMutex(lock);
        for (int m = 0; m < MUS_COUNT; m++) {
            Track *tr = &tracks[m];
            if (!tr->clip.data || (tr->gain <= 0 && tr->target <= 0)) continue;
            const float *d = tr->clip.data;
            int len = tr->clip.len;
            for (int i = 0; i < n; i++) {
                if (tr->gain < tr->target) tr->gain = fminf(tr->target, tr->gain + FADE_PER_FRAME);
                else if (tr->gain > tr->target) tr->gain = fmaxf(tr->target, tr->gain - FADE_PER_FRAME);
                int ip = (int)tr->pos;
                if (ip >= len) { tr->pos -= len; ip = 0; }
                float v = muted ? 0 : d[ip] * tr->gain * tr->clip.vol;
                mixbuf[2 * i] += v * 0.7071f;
                mixbuf[2 * i + 1] += v * 0.7071f;
                tr->pos += 1;
            }
        }
        if (!muted)
            for (int v = 0; v < VOICES; v++) {
                Voice *vc = &voices[v];
                if (!vc->clip) continue;
                const float *d = vc->clip->data;
                int len = vc->clip->len;
                for (int i = 0; i < n; i++) {
                    int ip = (int)vc->pos;
                    if (ip >= len - 1) {
                        if (!vc->loop) { vc->clip = NULL; break; }
                        vc->pos -= len - 1;
                        ip = (int)vc->pos;
                    }
                    float fr = (float)(vc->pos - ip);
                    float s = d[ip] + (d[ip + 1] - d[ip]) * fr;
                    mixbuf[2 * i] += s * vc->gl;
                    mixbuf[2 * i + 1] += s * vc->gr;
                    vc->pos += vc->step;
                }
            }
        SDL_UnlockMutex(lock);
        for (int i = 0; i < 2 * n; i++) {     /* gentle soft clip */
            float x = mixbuf[i];
            mixbuf[i] = x / (1.0f + fabsf(x) * 0.25f);
        }
        SDL_PutAudioStreamData(st, mixbuf, n * 2 * (int)sizeof(float));
        frames -= n;
    }
}

int snd_init(const char *asset_dir)
{
    char dir[1536], path[2048];
    snprintf(dir, sizeof dir, "%s/sounds", asset_dir);
    snprintf(path, sizeof path, "%s/sounds.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(status, sizeof status, "no %.120s", path); return -1; }

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        snprintf(status, sizeof status, "audio: %s", SDL_GetError());
        fclose(f);
        return -1;
    }
    lock = SDL_CreateMutex();

    int loaded = 0, missing = 0;
    char line[512], ev[64], file[256];
    float vol;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, "%63s %255s %f", ev, file, &vol) != 3) continue;
        Clip *c = NULL;
        if (!strcmp(ev, "ambience")) c = &ambience;
        for (int i = 0; i < MUS_COUNT; i++)
            if (!strcmp(ev, MUSIC_EVENT[i])) c = &tracks[i].clip;
        for (int i = 0; i < SFX_COUNT; i++)
            if (!strcmp(ev, EVENT[i])) c = &clips[i];
        if (!c) continue;
        if (c->data) SDL_free(c->data);
        memset(c, 0, sizeof *c);
        if (load_clip(c, dir, file, vol) == 0) loaded++;
        else missing++;
    }
    fclose(f);
    for (size_t k = 0; k < sizeof DEFAULTS / sizeof DEFAULTS[0]; k++) {
        Clip *c = NULL;
        for (int i = 0; i < MUS_COUNT; i++)
            if (!strcmp(DEFAULTS[k].ev, MUSIC_EVENT[i])) c = &tracks[i].clip;
        for (int i = 0; i < SFX_COUNT; i++)
            if (!strcmp(DEFAULTS[k].ev, EVENT[i])) c = &clips[i];
        if (c && !c->data && load_clip(c, dir, DEFAULTS[k].file, DEFAULTS[k].vol) == 0) loaded++;
    }

    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, RATE };
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!stream) {
        snprintf(status, sizeof status, "audio device: %s", SDL_GetError());
        return -1;
    }
    tracks[MUS_GAME].gain = tracks[MUS_GAME].target = 1.0f;
    start_voice(&ambience, 1.0f, 1.0f, 0, true);
    SDL_ResumeAudioStreamDevice(stream);
    ready = true;
    snprintf(status, sizeof status, "%d sounds%s", loaded, missing ? " (some missing)" : "");
    return 0;
}

void snd_play(Sfx id, float vol, float pitch, float pan)
{
    if (!ready || id < 0 || id >= SFX_COUNT) return;
    start_voice(&clips[id], vol, pitch, pan, false);
}

void snd_toggle_mute(void)
{
    if (!ready) return;
    SDL_LockMutex(lock);
    muted = !muted;
    SDL_UnlockMutex(lock);
}

void snd_music(Music m)
{
    if (!ready || m < 0 || m >= MUS_COUNT) return;
    SDL_LockMutex(lock);
    for (int i = 0; i < MUS_COUNT; i++) {
        if (i == (int)m && tracks[i].gain <= 0 && m == MUS_TITLE) tracks[i].pos = 0;
        tracks[i].target = i == (int)m ? 1.0f : 0.0f;
    }
    SDL_UnlockMutex(lock);
}

bool snd_muted(void) { return muted; }

const char *snd_status(void)
{
    static char buf[200];
    snprintf(buf, sizeof buf, "%s%s", status, muted ? " (muted)" : "");
    return buf;
}

void snd_shutdown(void)
{
    if (stream) SDL_DestroyAudioStream(stream);
    stream = NULL;
    ready = false;
    for (int i = 0; i < SFX_COUNT; i++) { SDL_free(clips[i].data); clips[i].data = NULL; }
    for (int i = 0; i < MUS_COUNT; i++) { SDL_free(tracks[i].clip.data); memset(&tracks[i], 0, sizeof tracks[i]); }
    SDL_free(ambience.data); ambience.data = NULL;
    memset(voices, 0, sizeof voices);
    if (lock) SDL_DestroyMutex(lock);
    lock = NULL;
}
