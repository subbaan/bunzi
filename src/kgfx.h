/* Presents a Canvas through the kitty graphics protocol. */
#ifndef KGFX_H
#define KGFX_H

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"
#include "term.h"

typedef enum { TX_SHM, TX_DIRECT, TX_DIRECTZ, TX_COUNT } TxMode;
typedef enum { SC_SELF, SC_KITTY, SC_COUNT } ScaleMode;

typedef struct {
    TxMode    tx;
    ScaleMode sc;
    unsigned  sent;             /* frames handed to the terminal */
    int       scale;            /* integer upscale factor in use */
    int       out_w, out_h;     /* transmitted image size */
    size_t    last_bytes;       /* raw pixel bytes of last frame */
    size_t    last_wire;        /* bytes written to the tty for last frame */
    uint8_t  *pix;  size_t pixcap;
    uint8_t  *z;    size_t zcap;
    char     *b64;  size_t b64cap;
    const char *pre;            /* emitted once inside the next frame's synchronized update */
} KGfx;

const char *kgfx_tx_name(TxMode m);
const char *kgfx_sc_name(ScaleMode m);
int  kgfx_parse_tx(const char *s, TxMode *out);
int  kgfx_parse_sc(const char *s, ScaleMode *out);

void kgfx_init(KGfx *g, TxMode tx, ScaleMode sc);
int  kgfx_present(KGfx *g, const Canvas *cv, const TermSize *ts);
int  kgfx_present_rgba(KGfx *g, const uint32_t *argb, int w, int h, const char *pre);
void kgfx_cleanup(KGfx *g);

#endif
