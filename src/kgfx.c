#include "kgfx.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* shm names are recycled through a ring so anything kitty never consumed
 * gets unlinked by us eventually (kitty unlinks the ones it reads). */
#define SHM_RING 16
#define CHUNK    4096

static const char *TX_NAMES[TX_COUNT] = { "shm", "direct", "directz" };
static const char *SC_NAMES[SC_COUNT] = { "self", "kitty" };

const char *kgfx_tx_name(TxMode m) { return m < TX_COUNT ? TX_NAMES[m] : "?"; }
const char *kgfx_sc_name(ScaleMode m) { return m < SC_COUNT ? SC_NAMES[m] : "?"; }

int kgfx_parse_tx(const char *s, TxMode *out)
{
    for (int i = 0; i < TX_COUNT; i++)
        if (!strcmp(s, TX_NAMES[i])) { *out = (TxMode)i; return 0; }
    return -1;
}

int kgfx_parse_sc(const char *s, ScaleMode *out)
{
    for (int i = 0; i < SC_COUNT; i++)
        if (!strcmp(s, SC_NAMES[i])) { *out = (ScaleMode)i; return 0; }
    return -1;
}

void kgfx_init(KGfx *g, TxMode tx, ScaleMode sc)
{
    memset(g, 0, sizeof *g);
    g->tx = tx;
    g->sc = sc;
}

static void grow(void **buf, size_t *cap, size_t need)
{
    if (*cap >= need) return;
    void *nb = realloc(*buf, need);
    if (!nb) abort();
    *buf = nb;
    *cap = need;
}

static void shm_name(char *out, size_t n, unsigned seq)
{
    snprintf(out, n, "/bunzi-%d-%u", (int)getpid(), seq);
}

/* Canvas -> packed RGB, nearest-neighbour upscaled by s. */
static void blit_rgb(uint8_t *dst, const Canvas *cv, int s)
{
    size_t rowbytes = (size_t)cv->w * s * 3;
    for (int y = 0; y < cv->h; y++) {
        uint8_t *d0 = dst + (size_t)y * s * rowbytes, *d = d0;
        const uint32_t *src = cv->px + (size_t)y * cv->w;
        if (s == 1) {
            for (int x = 0; x < cv->w; x++) {
                uint32_t c = src[x];
                *d++ = (uint8_t)(c >> 16);
                *d++ = (uint8_t)(c >> 8);
                *d++ = (uint8_t)c;
            }
            continue;
        }
        for (int x = 0; x < cv->w; x++) {
            uint32_t c = src[x];
            uint8_t r = (uint8_t)(c >> 16), gg = (uint8_t)(c >> 8), b = (uint8_t)c;
            for (int k = 0; k < s; k++) { *d++ = r; *d++ = gg; *d++ = b; }
        }
        for (int k = 1; k < s; k++) memcpy(d0 + (size_t)k * rowbytes, d0, rowbytes);
    }
}

static int send_shm(KGfx *g, const Canvas *cv, int s, size_t nbytes, const char *keys)
{
    unsigned seq = g->sent;
    char name[64];
    if (seq >= SHM_RING) {
        shm_name(name, sizeof name, seq - SHM_RING);
        shm_unlink(name);
    }
    shm_name(name, sizeof name, seq);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 && errno == EEXIST) {
        shm_unlink(name);
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)nbytes) < 0) {
        close(fd);
        shm_unlink(name);
        return -1;
    }
    uint8_t *m = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        shm_unlink(name);
        return -1;
    }
    blit_rgb(m, cv, s);
    munmap(m, nbytes);

    char b64[128];
    b64_encode((const uint8_t *)name, strlen(name), b64);
    out_printf("\x1b_G%s,t=s,S=%zu;%s\x1b\\", keys, nbytes, b64);
    return 0;
}

static int send_direct(KGfx *g, const Canvas *cv, int s, size_t nbytes, const char *keys)
{
    grow((void **)&g->pix, &g->pixcap, nbytes);
    blit_rgb(g->pix, cv, s);

    const uint8_t *payload = g->pix;
    size_t plen = nbytes;
    const char *extra = "";
    if (g->tx == TX_DIRECTZ) {
        uLongf zl = compressBound((uLong)nbytes);
        grow((void **)&g->z, &g->zcap, zl);
        if (compress2(g->z, &zl, g->pix, (uLong)nbytes, 1) != Z_OK) return -1;
        payload = g->z;
        plen = zl;
        extra = ",o=z";
    }

    size_t blen = 4 * ((plen + 2) / 3);
    grow((void **)&g->b64, &g->b64cap, blen + 1);
    b64_encode(payload, plen, g->b64);

    for (size_t off = 0; off < blen; off += CHUNK) {
        size_t n = blen - off < CHUNK ? blen - off : CHUNK;
        int more = off + n < blen;
        if (off == 0) out_printf("\x1b_G%s%s,t=d,m=%d;", keys, extra, more);
        else          out_printf("\x1b_Gm=%d;", more);
        out_bytes(g->b64 + off, n);
        out_str("\x1b\\");
    }
    return 0;
}

int kgfx_present(KGfx *g, const Canvas *cv, const TermSize *ts)
{
    if (ts->xpix <= 0 || ts->ypix <= 0 || ts->cellw <= 0 || ts->cellh <= 0) return -1;

    int s = ts->xpix / cv->w;
    if (ts->ypix / cv->h < s) s = ts->ypix / cv->h;
    if (s < 1) s = 1;

    int dispw = cv->w * s, disph = cv->h * s;
    int iw, ih, pc = 0, pr = 0, bs;
    if (g->sc == SC_SELF) {
        iw = dispw;
        ih = disph;
        bs = s;
    } else {
        /* kitty stretches into a whole-cell rectangle, so the factor may be slightly off-integer */
        iw = cv->w;
        ih = cv->h;
        bs = 1;
        pc = (dispw + ts->cellw / 2) / ts->cellw;
        pr = (disph + ts->cellh / 2) / ts->cellh;
        if (pc > ts->cols) pc = ts->cols;
        if (pr > ts->rows) pr = ts->rows;
        if (pc < 1) pc = 1;
        if (pr < 1) pr = 1;
        dispw = pc * ts->cellw;
        disph = pr * ts->cellh;
    }

    int ox = (ts->xpix - dispw) / 2, oy = (ts->ypix - disph) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    int col = ox / ts->cellw, X = ox % ts->cellw;
    int row = oy / ts->cellh, Y = oy % ts->cellh;

    g->scale = s;
    g->out_w = iw;
    g->out_h = ih;
    size_t nbytes = (size_t)iw * ih * 3;
    g->last_bytes = nbytes;

    char keys[192];
    int kl = snprintf(keys, sizeof keys, "a=T,f=24,s=%d,v=%d,i=1,p=1,q=0,C=1,z=0", iw, ih);
    if (X)  kl += snprintf(keys + kl, sizeof keys - kl, ",X=%d", X);
    if (Y)  kl += snprintf(keys + kl, sizeof keys - kl, ",Y=%d", Y);
    if (pc) snprintf(keys + kl, sizeof keys - kl, ",c=%d,r=%d", pc, pr);

    out_str("\x1b[?2026h");
    if (g->pre) { out_str(g->pre); g->pre = NULL; }
    out_printf("\x1b[%d;%dH", row + 1, col + 1);
    int rc = g->tx == TX_SHM ? send_shm(g, cv, bs, nbytes, keys)
                             : send_direct(g, cv, bs, nbytes, keys);
    out_str("\x1b[?2026l");
    g->last_wire = out_flush();
    if (rc == 0) g->sent++;
    return rc;
}

/* Full-window transparent frame for the terminal transitions: f=32 RGBA at
 * the top-left cell, cursor saved and restored so the shell's screen keeps
 * its cursor. `pre` is emitted inside the same synchronized update (e.g. a
 * screen switch), so kitty shows both at once. Shared memory only. */
int kgfx_present_rgba(KGfx *g, const uint32_t *argb, int w, int h, const char *pre)
{
    size_t nbytes = (size_t)w * h * 4;
    unsigned seq = g->sent;
    char name[64];
    if (seq >= SHM_RING) {
        shm_name(name, sizeof name, seq - SHM_RING);
        shm_unlink(name);
    }
    shm_name(name, sizeof name, seq);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 && errno == EEXIST) {
        shm_unlink(name);
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)nbytes) < 0) { close(fd); shm_unlink(name); return -1; }
    uint8_t *m = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { shm_unlink(name); return -1; }
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
        uint32_t c = argb[i];
        m[i * 4] = (uint8_t)(c >> 16);
        m[i * 4 + 1] = (uint8_t)(c >> 8);
        m[i * 4 + 2] = (uint8_t)c;
        m[i * 4 + 3] = (uint8_t)(c >> 24);
    }
    munmap(m, nbytes);

    char b64[128];
    b64_encode((const uint8_t *)name, strlen(name), b64);
    out_str("\x1b[?2026h");
    if (pre) out_str(pre);
    out_printf("\x1b" "7\x1b[1;1H\x1b_Ga=T,f=32,s=%d,v=%d,i=1,p=1,q=0,C=1,z=0,t=s,S=%zu;%s\x1b\\" "\x1b" "8",
               w, h, nbytes, b64);
    out_str("\x1b[?2026l");
    g->last_bytes = nbytes;
    g->last_wire = out_flush();
    g->sent++;
    return 0;
}

void kgfx_cleanup(KGfx *g)
{
    char name[64];
    unsigned from = g->sent > SHM_RING ? g->sent - SHM_RING : 0;
    for (unsigned s = from; s < g->sent; s++) {
        shm_name(name, sizeof name, s);
        shm_unlink(name);
    }
    free(g->pix);
    free(g->z);
    free(g->b64);
    memset(g, 0, sizeof *g);
}
