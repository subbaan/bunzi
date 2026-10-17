/* bunzi: a pixel-art platformer drawn into a software framebuffer and
 * presented in the terminal through the kitty graphics protocol. */
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "gfx.h"
#include "kgfx.h"
#include "level.h"
#include "pad.h"
#include "results.h"
#include "sound.h"
#include "term.h"
#include "termfx.h"
#include "title.h"
#include "version.h"

#define SIM_HZ 240
#define MAX_INFLIGHT 2

static const int FPS_STEPS[] = { 30, 60, 90, 120, 144, 240 };
#define N_FPS (int)(sizeof FPS_STEPS / sizeof FPS_STEPS[0])

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static bool dir_has_assets(const char *dir)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/moonlit_ruins/gen/sprites.png", dir);
    return access(p, R_OK) == 0;
}

/* assets/ next to the executable (or one level up from build/), else ./assets */
static int find_assets(char *out, size_t n, const char *override)
{
    if (override) {
        snprintf(out, n, "%s", override);
        return dir_has_assets(out) ? 0 : -1;
    }
    char exe[PATH_MAX - 32];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;
        const char *rel[] = { "assets", "../assets" };
        for (int i = 0; i < 2; i++) {
            snprintf(out, n, "%s/%s", exe, rel[i]);
            if (dir_has_assets(out)) return 0;
        }
    }
    snprintf(out, n, "assets");
    return dir_has_assets(out) ? 0 : -1;
}

static void usage(FILE *f)
{
    fprintf(f,
        "Usage: %s [options]\n"
        "Bunzi, a pixel-art platformer for the kitty terminal.\n"
        "\n"
        "  -m, --mode=MODE    pixel transport: shm (default), direct, directz\n"
        "  -s, --scale=WHO    who upscales: self (default) or kitty\n"
        "  -f, --fps=N        target frame rate (default 60)\n"
        "  -a, --assets=DIR   asset directory (default: found next to the binary)\n"
        "      --probe        report terminal capabilities and exit\n"
        "      --bench=SECS   autoplay for SECS seconds, then print stats\n"
        "      --log=FILE     append probe results and run stats to FILE\n"
        "      --dump=FILE    render one frame to a PPM image and exit\n"
        "  -q, --mute         start with sound muted (N toggles in game)\n"
        "  -l, --level=N      skip the title screen and start at level N (1-%d)\n"
        "      --no-intro     skip the opening: by default the terminal's own text dissolves\n"
        "                     into the title screen and reassembles on quit (kitty remote\n"
        "                     control socket, read-only; a plain sweep without it)\n"
        "      --intro        play the opening even with --level\n"
        "      --show-fps     show the frame rate in the bottom-left corner\n"
        "      --check        check the levels for problems (e.g. unsupported hanging decor)\n"
        "  -h, --help         show this help\n"
        "  -V, --version      show version\n"
        "\n"
        "Title:    arrows/WASD or d-pad choose, Z/space/enter or A/start select,\n"
        "          esc/Q quit (gamepad B or back goes to QUIT).\n"
        "Keyboard: arrows/WASD move, Z/space/up jump (hold for height), X/shift run,\n"
        "          down+jump drops through platforms, R restart, tab debug overlay,\n"
        "          N mute, 1-9 jump to a level, M transport, S scaler, F fps,\n"
        "          esc/Q back to the title.\n"
        "Gamepad:  d-pad/left stick move, A jump, X/RB/RT run, back restart,\n"
        "          start debug overlay, back+start back to the title.\n"
        "\n"
        "Results:  arrows/WASD or d-pad choose, Z/space/enter or A select; initials:\n"
        "          type them or up/down, enter when done.\n"
        "\n"
        "Perfect clears (every coin) are ranked per level, by the time less 0.5s per\n"
        "enemy stomped, in $XDG_DATA_HOME/bunzi/scores.txt (~/.local/share/bunzi).\n"
        "\n"
        "BUNZI_DUMP_TITLE=SECS with --dump renders the title screen instead;\n"
        "BUNZI_DUMP_RESULTS=\"COINS KILLS SECS\" adds a results screen (scores read only).\n",
        PLAT_NAME, game_level_count());
}

static void print_caps(FILE *o, const TermCaps *c, const TermSize *ts, const PadState *ps)
{
    fprintf(o, "%s %s capability probe\n", PLAT_NAME, PLAT_VERSION);
    fprintf(o, "  window:              %dx%d px, %dx%d cells, cell %dx%d px\n",
            ts->xpix, ts->ypix, ts->cols, ts->rows, ts->cellw, ts->cellh);
    fprintf(o, "  keyboard protocol:   %s", c->kb_proto ? "yes" : "NO");
    if (c->kb_proto) fprintf(o, " (current flags %d)", c->kb_flags);
    fprintf(o, "\n  graphics protocol:   %s%s%s\n", c->gfx ? "yes" : "NO",
            c->gfx_err[0] ? " - " : "", c->gfx_err);
    fprintf(o, "  shared memory:       %s%s%s\n", c->gfx_shm ? "yes" : "NO",
            c->shm_err[0] ? " - " : "", c->shm_err);
    fprintf(o, "  synchronized output: %s\n", c->sync_output ? "yes" : "no");
    fprintf(o, "  gamepad:             %s\n", ps->connected ? ps->name : "none");
}

/* Hanging decor needs masonry, ground, a platform or a bridge directly above it. */
static int check_level(const char *assets, int level)
{
    char path[PATH_MAX + 64], err[256];
    game_level_path(level, assets, path, sizeof path);
    Level lv;
    if (level_load(&lv, path, err, sizeof err)) { fprintf(stderr, "%s: %s\n", PLAT_NAME, err); return 1; }
    const char *hangs = game_level_hangs(level);
    int problems = 0;
    for (int ty = 0; ty < lv.h; ty++)
        for (int tx = 0; tx < lv.w; tx++) {
            char c = level_ch(&lv, tx, ty), up = level_ch(&lv, tx, ty - 1);
            if (c != ' ' && strchr(hangs, c) && !strchr("#%=_", up)) {
                printf("%s: column %d row %d: '%c' hangs from nothing\n", path, tx, ty, c);
                problems++;
            }
        }
    if (lv.start_ty == 0 && level_ch(&lv, lv.start_tx, 0) != '@') {
        printf("%s: no '@' start position\n", path);
        problems++;
    }
    printf("%s: %d problem%s\n", path, problems, problems == 1 ? "" : "s");
    level_free(&lv);
    return problems ? 1 : 0;
}

static int dump_frame(const char *assets, int level, const char *path)
{
    char err[256];
    Game *g = game_create(assets, level, err, sizeof err);
    if (!g) { fprintf(stderr, "%s: %s\n", PLAT_NAME, err); return 1; }
    Canvas cv = { VIEW_W, VIEW_H, calloc(VIEW_W * VIEW_H, sizeof(uint32_t)) };
    const char *title_secs = getenv("BUNZI_DUMP_TITLE");
    if (title_secs) {
        Title *ti = title_create(assets, err, sizeof err);
        if (!ti) { fprintf(stderr, "%s: %s\n", PLAT_NAME, err); free(cv.px); game_destroy(g); return 1; }
        int lv = level;
        for (int i = 0; i < (int)(atof(title_secs) * SIM_HZ); i++) title_update(ti, NULL, 1.0f / SIM_HZ, &lv);
        title_render(ti, &cv);
        title_destroy(ti);
    } else {
        const char *tx = getenv("BUNZI_DUMP_TX");
        if (tx) game_teleport(g, atoi(tx));
        const char *secs = getenv("BUNZI_DUMP_SECS");
        int steps = (int)((secs ? atof(secs) : 2.5) * SIM_HZ);
        for (int i = 0; i < steps; i++) {
            Actions a = { .right = true, .jump = (i % 180) < 60, .jump_pressed = i % 180 == 0 };
            game_update(g, &a, 1.0f / SIM_HZ);
        }
        game_render(g, &cv);
        /* BUNZI_DUMP_RESULTS="coins kills secs": the results screen over it (scores read, never written) */
        const char *rs_env = getenv("BUNZI_DUMP_RESULTS");
        if (rs_env) {
            GameInfo gi;
            game_info(g, &gi);
            int coins = gi.coins_total, kills = 6;
            float secs_run = 83.42f;
            sscanf(rs_env, "%d %d %f", &coins, &kills, &secs_run);
            Results *res = results_create(true);
            if (res) {
                RunStats rs = { gi.level, gi.level_title, gi.level_hash, coins, gi.coins_total, kills,
                                secs_run, kills * 0.5f, false, true, true };
                results_begin(res, &rs);
                for (int i = 0; i < SIM_HZ; i++) results_update(res, NULL, 0, 1.0f / SIM_HZ);
                results_render(res, &cv);
                results_destroy(res);
            }
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); free(cv.px); game_destroy(g); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", VIEW_W, VIEW_H);
    for (int i = 0; i < VIEW_W * VIEW_H; i++) {
        uint32_t c = cv.px[i];
        fputc((int)(c >> 16 & 255), f);
        fputc((int)(c >> 8 & 255), f);
        fputc((int)(c & 255), f);
    }
    fclose(f);
    free(cv.px);
    game_destroy(g);
    return 0;
}

/* what is on screen: the title or a level */
typedef struct { Game *game; Title *title; bool on_title; } Scene;

static void scene_idle(Scene *sc, float dt)
{
    if (sc->on_title) {
        title_update(sc->title, NULL, dt, NULL);
    } else {
        Actions none = { 0 };
        game_update(sc->game, &none, dt);
    }
}

static void scene_render(Scene *sc, Canvas *cv)
{
    if (sc->on_title) title_render(sc->title, cv);
    else game_render(sc->game, cv);
}

/* Play the terminal transition. The scene keeps running (no input) so the
 * world is alive underneath. Any key or mouse event skips. Frames wait for
 * kitty's acknowledgement so a slow terminal drops frames instead of queueing. */
static void run_transition(TermFx *fx, bool outro, Scene *sc, Canvas *cv, KGfx *g, const char *first_pre,
                           FILE *logf)
{
    unsigned frames = 0, acked0 = g_input.gfx_ok, errs0 = g_input.gfx_err;
    termfx_start(fx, outro, cv->w, cv->h);
    float len = outro ? termfx_outro_len(fx) : termfx_intro_len(fx);
    const char *pre = first_pre;
    unsigned ev0 = g_input.events;
    double t0 = now_s(), last = t0;
    out_str("\x1b[?1000h");    /* mouse reporting: the wheel must not scroll the text away */
    out_flush();
    for (;;) {
        double now = now_s();
        float t = (float)(now - t0);
        bool skip = g_input.events != ev0 || g_quit || g_input.quit;
        if (skip) t = len;
        for (double s = last; s + 1.0 / SIM_HZ <= now; s += 1.0 / SIM_HZ) {
            scene_idle(sc, 1.0f / SIM_HZ);
            last = s + 1.0 / SIM_HZ;
        }
        scene_render(sc, cv);
        termfx_render(fx, outro, t > len ? len : t, cv);
        unsigned acks = g_input.gfx_ok + g_input.gfx_err;
        kgfx_present_rgba(g, fx->buf, fx->W, fx->H, pre);
        pre = NULL;
        frames++;
        /* developer-only (explicit env opt-in): save a few overlay frames */
        static const char *dump_dir;
        static int dumped;
        if (!dump_dir) dump_dir = getenv("BUNZI_FX_DUMP") ? getenv("BUNZI_FX_DUMP") : "";
        if (*dump_dir && dumped < 12 && t >= dumped * len / 10) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s_%02d.ppm", dump_dir, outro ? "outro" : "intro", dumped++);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", fx->W, fx->H);
                for (int i = 0; i < fx->W * fx->H; i++) {
                    uint32_t c = fx->buf[i];
                    int a = (int)(c >> 24), bgc = 0x20;
                    for (int k = 16; k >= 0; k -= 8)
                        fputc(((int)(c >> k & 255) * a + bgc * (255 - a)) / 255, f);
                }
                fclose(f);
            }
            if (dumped == 11) dumped = outro ? 12 : 0;
        }
        double wait_until = now_s() + 0.05;
        while (g_input.gfx_ok + g_input.gfx_err == acks && now_s() < wait_until) {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, 2) > 0) term_read_input();
        }
        if (t >= len) break;
        double next = now + 1.0 / 60;
        while (now_s() < next) {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, 1) > 0) term_read_input();
        }
    }
    out_str("\x1b[?1000l");
    out_flush();
    input_clear_edges();
    if (logf) {
        double el = now_s() - t0;
        fprintf(logf, "%s: %.2fs, %u frames (%.1f fps), %u acked, %u kitty errors, %dx%d RGBA\n",
                outro ? "outro" : "intro", el, frames, frames / el, g_input.gfx_ok - acked0,
                g_input.gfx_err - errs0, fx->W, fx->H);
    }
}

int main(int argc, char **argv)
{
    TxMode tx = TX_SHM;
    ScaleMode sc = SC_SELF;
    int fps_idx = 1, fps_custom = 0, o;
    bool probe_only = false, check_only = false, start_muted = false, level_given = false, show_fps = false;
    int intro = -1;                     /* -1 default: on when the title screen opens */
    int level = 0;
    double bench = 0;
    const char *log_path = NULL, *dump_path = NULL, *assets_opt = NULL;

    static const struct option opts[] = {
        { "mode",    required_argument, 0, 'm' },
        { "scale",   required_argument, 0, 's' },
        { "fps",     required_argument, 0, 'f' },
        { "assets",  required_argument, 0, 'a' },
        { "probe",   no_argument,       0, 'P' },
        { "bench",   required_argument, 0, 'B' },
        { "log",     required_argument, 0, 'L' },
        { "dump",    required_argument, 0, 'D' },
        { "check",   no_argument,       0, 'C' },
        { "mute",    no_argument,       0, 'q' },
        { "level",   required_argument, 0, 'l' },
        { "intro",   no_argument,       0, 'I' },
        { "no-intro", no_argument,      0, 'N' },
        { "show-fps", no_argument,      0, 'S' },
        { "help",    no_argument,       0, 'h' },
        { "version", no_argument,       0, 'V' },
        { 0, 0, 0, 0 }
    };
    while ((o = getopt_long(argc, argv, "m:s:f:a:l:qhV", opts, NULL)) != -1) {
        switch (o) {
        case 'm':
            if (kgfx_parse_tx(optarg, &tx)) { fprintf(stderr, "%s: bad mode '%s'\n", PLAT_NAME, optarg); return 2; }
            break;
        case 's':
            if (kgfx_parse_sc(optarg, &sc)) { fprintf(stderr, "%s: bad scale '%s'\n", PLAT_NAME, optarg); return 2; }
            break;
        case 'f':
            fps_custom = atoi(optarg);
            if (fps_custom < 1 || fps_custom > 1000) { fprintf(stderr, "%s: bad fps '%s'\n", PLAT_NAME, optarg); return 2; }
            break;
        case 'a': assets_opt = optarg; break;
        case 'P': probe_only = true; break;
        case 'B': bench = atof(optarg); break;
        case 'L': log_path = optarg; break;
        case 'D': dump_path = optarg; break;
        case 'C': check_only = true; break;
        case 'q': start_muted = true; break;
        case 'I': intro = 1; break;
        case 'N': intro = 0; break;
        case 'S': show_fps = true; break;
        case 'l':
            level_given = true;
            level = atoi(optarg) - 1;
            if (level < 0 || level >= game_level_count()) {
                fprintf(stderr, "%s: bad level '%s' (1-%d)\n", PLAT_NAME, optarg, game_level_count());
                return 2;
            }
            break;
        case 'h': usage(stdout); return 0;
        case 'V': printf("%s %s\n", PLAT_NAME, PLAT_VERSION); return 0;
        default:  usage(stderr); return 2;
        }
    }
    int fps = fps_custom ? fps_custom : FPS_STEPS[fps_idx];

    char assets[PATH_MAX];
    bool have_assets = find_assets(assets, sizeof assets, assets_opt) == 0;
    if (check_only) {
        if (!have_assets) { fprintf(stderr, "%s: assets not found\n", PLAT_NAME); return 1; }
        int bad = 0;
        for (int i = 0; i < game_level_count(); i++) bad |= check_level(assets, i);
        return bad;
    }
    if (dump_path) {
        if (!have_assets) { fprintf(stderr, "%s: assets not found\n", PLAT_NAME); return 1; }
        return dump_frame(assets, level, dump_path);
    }

    PadState pad = { 0 };
    pad_init();     /* before raw mode: SDL must not see a half-configured tty */

    if (term_init()) {
        fprintf(stderr, "%s: stdin and stdout must be a terminal\n", PLAT_NAME);
        pad_shutdown();
        return 1;
    }
    atexit(term_restore);

    TermCaps caps;
    TermSize ts;
    int probed = term_probe(&caps, 2.0);
    term_get_size(&ts);
    pad_poll(&pad);
    FILE *logf = log_path ? fopen(log_path, "a") : NULL;
    if (logf) print_caps(logf, &caps, &ts, &pad);

    const char *fatal = NULL;
    char err[512] = "";
    if (probe_only) {
        term_restore();
        if (probed) printf("warning: terminal did not answer the probe in time\n");
        print_caps(stdout, &caps, &ts, &pad);
        pad_shutdown();
        if (logf) fclose(logf);
        return caps.gfx ? 0 : 1;
    }
    if (!caps.gfx) fatal = "this terminal does not support the kitty graphics protocol";
    else if (ts.xpix <= 0) fatal = "terminal does not report its pixel size";
    else if (!have_assets) fatal = "assets not found (use --assets DIR)";
    Game *game = fatal ? NULL : game_create(assets, level, err, sizeof err);
    if (!fatal && !game) fatal = err;
    if (fatal) {
        term_restore();
        if (!caps.gfx) print_caps(stdout, &caps, &ts, &pad);
        fprintf(stderr, "%s: %s\n", PLAT_NAME, fatal);
        pad_shutdown();
        if (logf) fclose(logf);
        return 1;
    }
    if (tx == TX_SHM && !caps.gfx_shm) tx = TX_DIRECTZ;
    /* the title screen, unless a level was asked for (a missing title is not fatal) */
    Title *title = level_given || bench > 0 ? NULL : title_create(assets, err, sizeof err);
    if (!title && logf && !level_given && bench <= 0) fprintf(logf, "title: %s\n", err);
    Scene scene = { game, title, title != NULL };
    Results *res = results_create(bench > 0);
    bool in_res = false;                /* the results screen is up over a cleared level */
    int menu_level = level;
    snd_init(assets);
    if (start_muted || bench > 0) snd_toggle_mute();    /* benchmarks stay silent */
    snd_music(scene.on_title ? MUS_TITLE : MUS_GAME);

    Canvas cv = { VIEW_W, VIEW_H, calloc(VIEW_W * VIEW_H, sizeof(uint32_t)) };
    KGfx g;
    kgfx_init(&g, tx, sc);

    /* experimental terminal transition: needs shared memory for full-window frames */
    TermFx fx;
    if (intro < 0) intro = title != NULL;
    bool fx_on = intro && bench <= 0 && caps.gfx_shm && termfx_capture(&fx, &ts) == 0;
    if (intro && logf) fprintf(logf, "intro: %s\n", fx_on ? fx.status : "unavailable");
    if (fx_on) {
        out_str("\x1b[?25l");
        out_flush();
        if (title) title_reset(title, true);     /* the logo waits for the world to appear */
        run_transition(&fx, false, &scene, &cv, &g, NULL, logf);
        if (title) title_hold(title, false);
        /* drop the main-screen image and enter the game screen together with
         * the first game frame, in one synchronized update */
        static char sw[128];
        snprintf(sw, sizeof sw, "\x1b_Ga=d,d=A,q=2\x1b\\%s", TERM_GAME_SEQ);
        g.pre = sw;
    } else {
        term_enter_game();
    }

    const float DT = 1.0f / SIM_HZ;
    double t0 = now_s(), next = t0, sim_t = t0, last_ack_t = t0;
    double win_t = t0, win_render = 0, win_send = 0;
    unsigned win_frames = 0, win_acks = 0, last_acks = 0;
    unsigned tot_frames = 0, tot_acks = 0, drops = 0, stalls = 0, send_fail = 0;
    double disp_loop = 0, disp_kitty = 0, disp_render = 0, disp_send = 0;
    int inflight = 0;
    bool hud = false, pending_jump = false, quit = false;
    bool pad_was[4] = { 0 };            /* d-pad last frame, for menu edges */

    while (!g_quit && !g_input.quit) {
        for (;;) {
            double rem = next - now_s();
            if (rem <= 0) break;
            struct timespec tw = { (time_t)rem, (long)((rem - (time_t)rem) * 1e9) };
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (ppoll(&pfd, 1, &tw, NULL) > 0) term_read_input();
            if (g_quit) break;
        }
        term_read_input();
        pad_poll(&pad);
        double t = now_s(), period = 1.0 / fps;
        next += period;
        if (next < t) next = t + period;

        if (g_resized) {
            g_resized = 0;
            term_get_size(&ts);
            out_str("\x1b[2J");
            out_flush();
        }

        const bool *pr = g_input.pressed, *d = g_input.down;
        bool pad_now[4] = { pad.up, pad.down, pad.left, pad.right }, pad_edge[4];
        for (int k = 0; k < 4; k++) { pad_edge[k] = pad_now[k] && !pad_was[k]; pad_was[k] = pad_now[k]; }
        TitleInput ti = { 0 };
        char typed = 0;
        bool typing = in_res && results_typing(res);
        if (typing) {                       /* initials: letter keys type, arrows and the pad edit */
            for (int c = 'a'; c <= 'z' && !typed; c++) if (pr[c]) typed = (char)(c - 32);
            if (pr[127] || pr[8]) typed = '\b';
            if (pr[K_ENTER]) typed = '\n';
            ti = (TitleInput){
                .up = pr[K_UP] || pad_edge[0], .down = pr[K_DOWN] || pad_edge[1],
                .left = pr[K_LEFT] || pad_edge[2], .right = pr[K_RIGHT] || pad_edge[3],
                .confirm = pad.jump_pressed || pad.start_pressed, .back = pad.cancel_pressed,
            };
        } else if (in_res) {
            ti = (TitleInput){
                .up = pr[K_UP] || pr['w'] || pad_edge[0], .down = pr[K_DOWN] || pr['s'] || pad_edge[1],
                .left = pr[K_LEFT] || pr['a'] || pad_edge[2], .right = pr[K_RIGHT] || pr['d'] || pad_edge[3],
                .confirm = pr[K_ENTER] || pr[' '] || pr['z'] || pad.jump_pressed || pad.start_pressed,
                .back = pad.cancel_pressed,
            };
        }
        if (scene.on_title) {
            if (pr[K_ESC] || pr['q']) break;
            ti = (TitleInput){
                .up = pr[K_UP] || pr['w'] || pad_edge[0], .down = pr[K_DOWN] || pr['s'] || pad_edge[1],
                .left = pr[K_LEFT] || pr['a'] || pad_edge[2], .right = pr[K_RIGHT] || pr['d'] || pad_edge[3],
                .confirm = pr[K_ENTER] || pr[' '] || pr['z'] || pad.jump_pressed || pad.start_pressed,
                .back = pad.cancel_pressed || pad.back_pressed,
            };
        } else {
            bool leave = pr[K_ESC] || (pr['q'] && !typing) || (pad.start && pad.back && (pad.start_pressed || pad.back_pressed));
            if (leave && !title) break;
            if (leave) {                    /* back to the title, on the level we were playing */
                GameInfo gi;
                game_info(game, &gi);
                menu_level = gi.level;
                scene.on_title = true;
                in_res = false;
                title_reset(title, false);
                snd_music(MUS_TITLE);
                input_clear_edges();
                continue;
            }
            if (!in_res) {
                if (pr['s'] && !d['a'] && !d['d']) g.sc = (ScaleMode)((g.sc + 1) % SC_COUNT);
                if (pr[K_TAB] || (pad.start_pressed && !pad.back)) hud = !hud;
                if (pr['r'] || (pad.back_pressed && !pad.start)) game_restart(game);
                for (int i = 0; i < 9 && i < game_level_count(); i++)
                    if (pr['1' + i]) game_set_level(game, i, err, sizeof err);
            }
        }
        if (pr['m'] && !typing) {      /* while typing initials, letters are letters */
            do g.tx = (TxMode)((g.tx + 1) % TX_COUNT); while (g.tx == TX_SHM && !caps.gfx_shm);
        }
        if (pr['f'] && !typing) { fps_idx = (fps_idx + 1) % N_FPS; fps = FPS_STEPS[fps_idx]; }
        if (pr['n'] && !typing) snd_toggle_mute();
        if (scene.on_title && pr[K_TAB]) hud = !hud;

        Actions act = {
            .left  = d[K_LEFT] || d['a'] || pad.left,
            .right = d[K_RIGHT] || d['d'] || pad.right,
            .up    = d[K_UP] || d['w'] || pad.up,
            .down  = d[K_DOWN] || d['s'] || pad.down,
            .jump  = d['z'] || d[' '] || d[K_UP] || d['w'] || pad.jump,
            .run   = d['x'] || d[K_SHIFT] || pad.run,
        };
        pending_jump |= pr['z'] || pr[' '] || pr[K_UP] || pr['w'] || pad.jump_pressed;
        if (in_res) act = (Actions){ 0 };   /* the bunny waits at the exit */
        if (bench > 0) {
            if (t - t0 >= bench) break;
            act.right = act.run = true;
            act.jump = fmod(t - t0, 0.8) < 0.3;
            if (fmod(t - t0, 0.8) < period) pending_jump = true;
        }
        input_clear_edges();

        if (t - sim_t > 0.25) sim_t = t - 0.25;
        bool first = true;
        while (sim_t + DT <= t) {
            if (scene.on_title) {
                TitleAction ta = title_update(title, first ? &ti : NULL, DT, &menu_level);
                if (ta == TA_QUIT) { quit = true; break; }
                if (ta == TA_START) {
                    if (game_set_level(game, menu_level, err, sizeof err) == 0) {
                        scene.on_title = false;
                        snd_music(MUS_GAME);
                        pending_jump = false;
                    }
                    break;
                }
            } else {
                act.jump_pressed = pending_jump && !in_res;
                game_update(game, &act, DT);
                pending_jump = false;
                GameInfo gi;
                game_info(game, &gi);
                if (res && !in_res && gi.clear && gi.clear_t > 0.7f) {
                    RunStats rs = { gi.level, gi.level_title, gi.level_hash, gi.coins, gi.coins_total,
                                    gi.kills, gi.time, gi.bonus, gi.practice || bench > 0,
                                    true, title != NULL };
                    results_begin(res, &rs);
                    in_res = true;
                }
                if (in_res) {
                    ResultsAction ra = results_update(res, first ? &ti : NULL, first ? typed : 0, DT);
                    if (bench > 0 && gi.clear_t > 2.0f) ra = RA_NEXT;
                    if (ra == RA_RETRY) { in_res = false; game_restart(game); }
                    else if (ra == RA_NEXT) {
                        in_res = false;
                        if (game_set_level(game, (gi.level + 1) % game_level_count(), err, sizeof err))
                            game_restart(game);
                    } else if (ra == RA_TITLE && title) {
                        in_res = false;
                        menu_level = gi.level;
                        scene.on_title = true;
                        title_reset(title, false);
                        snd_music(MUS_TITLE);
                        break;
                    }
                }
            }
            first = false;
            sim_t += DT;
        }
        if (quit) break;

        double r0 = now_s();
        scene_render(&scene, &cv);
        if (in_res && !scene.on_title) results_render(res, &cv);
        if (show_fps && !hud) {         /* frames kitty actually showed last second */
            char fb[24];
            if (disp_kitty > 0) snprintf(fb, sizeof fb, "%.0f FPS", disp_kitty);
            else snprintf(fb, sizeof fb, "-- FPS");
            uint32_t c = disp_kitty >= fps * 0.95 ? 0x9ad04a : disp_kitty > 0 ? 0xffb040 : 0xa8a0d0;
            gfx_text(&cv, 4, VIEW_H - 8, fb, 0x0a0618);
            gfx_text(&cv, 3, VIEW_H - 9, fb, c);
        }
        if (hud) {
            uint32_t W = 0xe8ecff, Y = 0xffd860, G = 0x9ad04a, R = 0xff6060;
            GameInfo gi;
            game_info(game, &gi);
            gfx_tint(&cv, 0, VIEW_H - 40, VIEW_W, 40, 0x000000, 140);
            int y = VIEW_H - 37;
            gfx_textf(&cv, 3, y, Y, "%s V%s   PAD: %s   SOUND: %s", PLAT_NAME, PLAT_VERSION,
                      pad.connected ? pad.name : "NONE", snd_status());
            gfx_textf(&cv, 3, y + 7, W, "TX %-7s SCALE %-5s X%d  IMAGE %dX%d  WINDOW %dX%d  TARGET %d FPS",
                      kgfx_tx_name(g.tx), kgfx_sc_name(g.sc), g.scale, g.out_w, g.out_h,
                      ts.xpix, ts.ypix, fps);
            gfx_textf(&cv, 3, y + 14, disp_kitty >= fps * 0.95 ? G : R,
                      "LOOP %5.1f FPS  KITTY ACK %5.1f FPS  INFLIGHT %d  DROPS %u  STALLS %u",
                      disp_loop, disp_kitty, inflight, drops, stalls);
            gfx_textf(&cv, 3, y + 21, W, "FRAME %.2fMB  WIRE %zuB  RENDER %.2fMS  SEND %.2fMS",
                      g.last_bytes / 1048576.0, g.last_wire, disp_render, disp_send);
            gfx_textf(&cv, 3, y + 28, W, "L%d %s  POS %.0f,%.0f  VEL %+4.0f,%+4.0f %s  %s%s%s%s%s",
                      gi.level + 1, gi.state, gi.x, gi.y, gi.vx, gi.vy, gi.ground ? "GROUND" : "AIR",
                      act.left ? "L" : "-", act.right ? "R" : "-", act.down ? "D" : "-",
                      act.jump ? "J" : "-", act.run ? "RUN" : "");
            if (!caps.kb_proto)
                gfx_textf(&cv, 3, VIEW_H - 47, R, "NO KITTY KEYBOARD PROTOCOL: NO KEY RELEASE EVENTS");
            else if (g_input.gfx_last_err[0])
                gfx_textf(&cv, 3, VIEW_H - 47, R, "KITTY: %.60s", g_input.gfx_last_err);
        }
        double r1 = now_s();

        unsigned acks = g_input.gfx_ok + g_input.gfx_err;
        if (acks != last_acks) {
            unsigned na = acks - last_acks;
            inflight -= (int)na;
            if (inflight < 0) inflight = 0;
            win_acks += na;
            tot_acks += na;
            last_acks = acks;
            last_ack_t = t;
        }
        if (inflight >= MAX_INFLIGHT && t - last_ack_t > 0.5) {
            inflight = 0;
            stalls++;
            last_ack_t = t;
        }
        if (inflight < MAX_INFLIGHT) {
            if (kgfx_present(&g, &cv, &ts) == 0) {
                inflight++;
                win_frames++;
                tot_frames++;
            } else {
                send_fail++;
            }
        } else {
            drops++;
        }
        double r2 = now_s();
        win_render += r1 - r0;
        win_send += r2 - r1;

        if (t - win_t >= 1.0) {
            double el = t - win_t;
            disp_loop = win_frames / el;
            disp_kitty = win_acks / el;
            disp_render = win_frames ? win_render / win_frames * 1000 : 0;
            disp_send = win_frames ? win_send / win_frames * 1000 : 0;
            win_frames = win_acks = 0;
            win_render = win_send = 0;
            win_t = t;
        }
    }

    if (fx_on && !g_quit && !g_input.quit) {
        /* back to the shell's screen, covered by the world, then reassemble */
        scene_render(&scene, &cv);
        run_transition(&fx, true, &scene, &cv, &g,
                       "\x1b_Ga=d,d=A,q=2\x1b\\\x1b[<u\x1b[?1004l\x1b[?1049l\x1b[?25l", logf);
        out_str("\x1b_Ga=d,d=A,q=2\x1b\\");
        out_flush();
    }
    if (fx_on) termfx_free(&fx);

    double el = now_s() - t0;
    TxMode tx_final = g.tx;
    ScaleMode sc_final = g.sc;
    int scale_final = g.scale, w_final = g.out_w, h_final = g.out_h;
    GameInfo gi;
    game_info(game, &gi);
    kgfx_cleanup(&g);
    term_restore();
    char sndst[200];
    snprintf(sndst, sizeof sndst, "%s", snd_status());
    snd_shutdown();
    pad_shutdown();
    game_destroy(game);
    title_destroy(title);
    results_destroy(res);
    free(cv.px);

    char sum[900];
    snprintf(sum, sizeof sum,
             "%s %s: tx=%s scale=%s x%d image=%dx%d window=%dx%d target=%dfps | %.1fs, "
             "%u sent (%.1f fps), %u acked (%.1f fps), %u dropped, %u stalls, "
             "%u send failures, %u kitty errors, last render %.2fms send %.2fms | "
             "level %d coins %d/%d hearts %d | sound %s\n",
             PLAT_NAME, PLAT_VERSION, kgfx_tx_name(tx_final), kgfx_sc_name(sc_final), scale_final,
             w_final, h_final, ts.xpix, ts.ypix, fps, el, tot_frames, tot_frames / el,
             tot_acks, tot_acks / el, drops, stalls, send_fail, g_input.gfx_err,
             disp_render, disp_send, gi.level + 1, gi.coins, gi.coins_total, gi.hearts, sndst);
    if (bench > 0 || logf) fputs(sum, stdout);
    if (logf) fputs(sum, logf);
    if (g_input.gfx_last_err[0]) {
        printf("last kitty error: %s\n", g_input.gfx_last_err);
        if (logf) fprintf(logf, "last kitty error: %s\n", g_input.gfx_last_err);
    }
    if (logf) fclose(logf);
    return 0;
}
