#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

InputState g_input;
volatile sig_atomic_t g_quit, g_resized;

static struct termios orig_tio;
static volatile sig_atomic_t raw_on;
static TermCaps *probe_caps;
static bool da_seen;

/* Pop keyboard flags before leaving the alt screen: kitty keeps one stack per screen. */
static const char RESTORE_SEQ[] =
    "\x1b_Ga=d,d=A,q=2\x1b\\"
    "\x1b[<u"
    "\x1b[?1004l"
    "\x1b[0m\x1b[?25h"
    "\x1b[?1049l"
    "\x1b[?25h";

/* ---------------------------------------------------------------- output */

static char  *obuf;
static size_t olen, ocap;

void out_bytes(const void *p, size_t n)
{
    if (olen + n > ocap) {
        size_t nc = ocap ? ocap : 65536;
        while (nc < olen + n) nc *= 2;
        char *nb = realloc(obuf, nc);
        if (!nb) abort();
        obuf = nb;
        ocap = nc;
    }
    memcpy(obuf + olen, p, n);
    olen += n;
}

void out_str(const char *s) { out_bytes(s, strlen(s)); }

void out_printf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = sizeof tmp - 1;
    out_bytes(tmp, (size_t)n);
}

size_t out_flush(void)
{
    size_t off = 0, total = olen;
    while (off < olen) {
        ssize_t w = write(STDOUT_FILENO, obuf + off, olen - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd p = { STDOUT_FILENO, POLLOUT, 0 };
                poll(&p, 1, 100);
                continue;
            }
            break;
        }
        off += (size_t)w;
    }
    olen = 0;
    return total;
}

size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = T[v >> 18];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        out[o++] = T[v >> 18];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------- terminal */

void term_restore(void)
{
    if (!raw_on) return;
    raw_on = 0;
    ssize_t r = write(STDOUT_FILENO, RESTORE_SEQ, sizeof RESTORE_SEQ - 1);
    (void)r;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
}

static void on_quit_signal(int sig) { (void)sig; g_quit = 1; }
static void on_winch(int sig) { (void)sig; g_resized = 1; }

static void on_fatal_signal(int sig)
{
    term_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

int term_init(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &orig_tio) < 0) return -1;

    struct termios t = orig_tio;
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag = (t.c_cflag & ~(CSIZE | PARENB)) | CS8;
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0) return -1;
    raw_on = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_fatal_signal;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    return 0;
}

/* alt screen, hide cursor, clear, focus events, keyboard flags 1|2|8:
 * disambiguate, report event types (press/repeat/release), all keys as escapes */
const char TERM_GAME_SEQ[] = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[?1004h\x1b[>11u";

void term_enter_game(void)
{
    out_str(TERM_GAME_SEQ);
    out_flush();
}

int term_get_size(TermSize *ts)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) < 0) return -1;
    ts->cols  = w.ws_col;
    ts->rows  = w.ws_row;
    ts->xpix  = w.ws_xpixel;
    ts->ypix  = w.ws_ypixel;
    ts->cellw = ts->cols ? ts->xpix / ts->cols : 0;
    ts->cellh = ts->rows ? ts->ypix / ts->rows : 0;
    return 0;
}

/* ---------------------------------------------------------------- input */

static unsigned char ibuf[16384];
static size_t        ilen;

void input_clear_edges(void)
{
    memset(g_input.pressed, 0, sizeof g_input.pressed);
    memset(g_input.released, 0, sizeof g_input.released);
}

static void key_event(int key, int ev)
{
    if (key < 0 || key >= K_MAX) return;
    if (ev == 3) {
        if (g_input.down[key]) g_input.released[key] = true;
        g_input.down[key] = false;
    } else {
        if (!g_input.down[key] && ev == 1) g_input.pressed[key] = true;
        g_input.down[key] = true;
    }
    g_input.events++;
}

static int map_code(int c)
{
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c >= 0 && c < 128) return c;
    switch (c) {
    case 57441: case 57447: return K_SHIFT;
    case 57442: case 57448: return K_CTRL;
    case 57443: case 57449: return K_ALT;
    }
    return -1;
}

static void handle_csi(const unsigned char *p, size_t n, unsigned char inter,
                       unsigned char fin)
{
    unsigned char lead = 0;
    if (n && (p[0] == '?' || p[0] == '>' || p[0] == '<' || p[0] == '=')) {
        lead = p[0];
        p++;
        n--;
    }

    long f[8][4];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) f[i][j] = -1;
    int np = 0, ns = 0;
    long cur = -1;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = i < n ? p[i] : ';';
        if (c >= '0' && c <= '9') {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 10000000) cur = 10000000;
        } else if (c == ':' || c == ';') {
            if (np < 8 && ns < 4) f[np][ns] = cur;
            cur = -1;
            if (c == ':') ns++;
            else { np++; ns = 0; }
        }
    }

    if (lead == '?') {
        if (fin == 'u' && probe_caps) {
            probe_caps->kb_proto = true;
            probe_caps->kb_flags = (int)(f[0][0] < 0 ? 0 : f[0][0]);
        } else if (fin == 'c') {
            da_seen = true;
        } else if (fin == 'y' && inter == '$' && f[0][0] == 2026 && probe_caps) {
            probe_caps->sync_output = f[1][0] >= 1 && f[1][0] <= 4;
        }
        return;
    }
    if (lead || inter) return;

    if (fin == 'O' && n == 0) {         /* focus lost: we will never see the releases */
        memset(g_input.down, 0, sizeof g_input.down);
        return;
    }
    if (fin == 'I' && n == 0) return;   /* focus gained */

    int ev = f[1][1] > 0 ? (int)f[1][1] : 1;
    int mods = f[1][0] > 0 ? (int)f[1][0] - 1 : 0;
    int key;
    switch (fin) {
    case 'u': key = map_code((int)f[0][0]); break;
    case 'A': key = K_UP; break;
    case 'B': key = K_DOWN; break;
    case 'C': key = K_RIGHT; break;
    case 'D': key = K_LEFT; break;
    default: return;
    }
    if (key == 'c' && (mods & 4) && ev == 1) g_input.quit = true;
    key_event(key, ev);
}

static void handle_apc(const unsigned char *s, size_t n)
{
    if (n < 1 || s[0] != 'G') return;
    char buf[256];
    size_t m = n - 1 < sizeof buf - 1 ? n - 1 : sizeof buf - 1;
    memcpy(buf, s + 1, m);
    buf[m] = 0;
    char *msg = strchr(buf, ';');
    if (!msg) return;
    *msg++ = 0;

    int id = -1;
    for (char *k = buf; k && *k; ) {
        if (k[0] == 'i' && k[1] == '=') { id = atoi(k + 2); break; }
        k = strchr(k, ',');
        if (k) k++;
    }
    bool ok = strcmp(msg, "OK") == 0;

    if (id == 31 && probe_caps) {
        probe_caps->gfx = ok;
        if (!ok) snprintf(probe_caps->gfx_err, sizeof probe_caps->gfx_err, "%s", msg);
    } else if (id == 32 && probe_caps) {
        probe_caps->gfx_shm = ok;
        if (!ok) snprintf(probe_caps->shm_err, sizeof probe_caps->shm_err, "%s", msg);
    } else if (id == 1) {
        if (ok) g_input.gfx_ok++;
        else {
            g_input.gfx_err++;
            snprintf(g_input.gfx_last_err, sizeof g_input.gfx_last_err, "%s", msg);
        }
    }
}

static void handle_char(unsigned char c)
{
    /* Legacy bytes only arrive without the keyboard protocol: edges, no holds. */
    if (c == 3) { g_input.quit = true; return; }
    int k = map_code(c);
    if (k >= 0) g_input.pressed[k] = true;
}

static void parse_input(void)
{
    size_t i = 0;
    while (i < ilen) {
        unsigned char c = ibuf[i];
        if (c != 0x1b) { handle_char(c); i++; continue; }
        if (i + 1 >= ilen) break;
        unsigned char nx = ibuf[i + 1];
        if (nx == '[') {
            size_t j = i + 2;
            while (j < ilen && ibuf[j] >= 0x30 && ibuf[j] <= 0x3f) j++;
            size_t pend = j;
            while (j < ilen && ibuf[j] >= 0x20 && ibuf[j] <= 0x2f) j++;
            if (j >= ilen) break;
            unsigned char inter = j > pend ? ibuf[pend] : 0;
            handle_csi(ibuf + i + 2, pend - (i + 2), inter, ibuf[j]);
            i = j + 1;
        } else if (nx == '_') {
            size_t j = i + 2;
            bool found = false;
            for (; j + 1 < ilen; j++)
                if (ibuf[j] == 0x1b && ibuf[j + 1] == '\\') { found = true; break; }
            if (!found) break;
            handle_apc(ibuf + i + 2, j - (i + 2));
            i = j + 2;
        } else {
            handle_char(0x1b);
            i++;
        }
    }
    memmove(ibuf, ibuf + i, ilen - i);
    ilen -= i;
}

void term_read_input(void)
{
    for (;;) {
        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&p, 1, 0) <= 0) break;
        if (p.revents & (POLLHUP | POLLERR)) { g_quit = 1; break; }
        if (!(p.revents & POLLIN)) break;
        if (ilen >= sizeof ibuf) ilen = 0;  /* garbage overflow: drop it */
        ssize_t n = read(STDIN_FILENO, ibuf + ilen, sizeof ibuf - ilen);
        if (n <= 0) {
            if (n == 0) g_quit = 1;
            break;
        }
        ilen += (size_t)n;
        parse_input();
    }
}

static double mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int term_probe(TermCaps *caps, double timeout_s)
{
    memset(caps, 0, sizeof *caps);
    probe_caps = caps;
    da_seen = false;

    char name[64];
    snprintf(name, sizeof name, "/%s-probe-%d", "bunzi", (int)getpid());
    bool shm_made = false;
    int fd = shm_open(name, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd >= 0) {
        if (ftruncate(fd, 4) == 0) {
            void *m = mmap(NULL, 4, PROT_WRITE, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) {
                memset(m, 0xff, 4);
                munmap(m, 4);
                shm_made = true;
            }
        }
        close(fd);
    }

    out_str("\x1b[?u");
    out_str("\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\");
    if (shm_made) {
        char b64[128];
        b64_encode((const uint8_t *)name, strlen(name), b64);
        out_printf("\x1b_Gi=32,s=1,v=1,a=q,t=s,f=32;%s\x1b\\", b64);
    } else {
        snprintf(caps->shm_err, sizeof caps->shm_err, "shm_open failed locally");
    }
    out_str("\x1b[?2026$p");
    out_str("\x1b[c");     /* DA1: every terminal answers, so it ends the probe */
    out_flush();

    double deadline = mono_s() + timeout_s;
    while (!da_seen && !g_quit) {
        double rem = deadline - mono_s();
        if (rem <= 0) break;
        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        poll(&p, 1, (int)(rem * 1000) + 1);
        term_read_input();
    }
    if (shm_made) shm_unlink(name);   /* kitty normally unlinks it already */
    input_clear_edges();
    probe_caps = NULL;
    return da_seen ? 0 : -1;
}
