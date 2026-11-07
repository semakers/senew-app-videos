/* w5v.c — el flujo .w5v de la app «Videos»: anillo, cortar cuadros, elegir el que toca, decodificar y ampliar (w5v.h). Sin libc ni divisiones.
 * Lo compilan la app (videos.c) y el arnés del PC (prueba/prueba_w5v.c): el arnés prueba ESTE código, no una copia. */
#include "w5v.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/* ---- el anillo ---------------------------------------------------------------------------------------------------------------------- */
void w5v_anillo(W5vAnillo *a, u8 *b, u32 cap)
{
    memset(a, 0, sizeof *a);
    a->b = b; a->cap = cap; a->mask = cap - 1;
}
u32 w5v_hueco(const W5vAnillo *a, u8 **d)
{
    u32 libre = a->cap - a->n, w = (a->ini + a->n) & a->mask, seguido = a->cap - w;
    *d = a->b + w;
    return libre < seguido ? libre : seguido;
}
void w5v_escrito(W5vAnillo *a, u32 k)
{
    if (k > a->cap - a->n) k = a->cap - a->n;
    a->n += k; a->entrados += k;
}
u32 w5v_mirar(const W5vAnillo *a, u32 desde, void *d, u32 k)
{
    u32 p, k1;
    if (desde >= a->n) return 0;
    if (k > a->n - desde) k = a->n - desde;
    p = (a->ini + desde) & a->mask;
    k1 = a->cap - p;
    if (k1 > k) k1 = k;
    memcpy(d, a->b + p, k1);
    if (k > k1) memcpy((u8 *)d + k1, a->b, k - k1);
    return k;
}
void w5v_tirar(W5vAnillo *a, u32 k)
{
    if (k > a->n) k = a->n;
    a->ini = (a->ini + k) & a->mask; a->n -= k; a->salidos += k;
}

/* ---- la cabecera y los cuadros ------------------------------------------------------------------------------------------------------ */
int w5v_cabecera(W5vAnillo *a, W5vCab *c)
{
    static const u8 magia[4] = { 'W', '5', 'V', '1' };
    u8 h[W5V_CAB];
    u32 k = w5v_mirar(a, 0, h, W5V_CAB), i;
    for (i = 0; i < k && i < 4; i++) if (h[i] != magia[i]) return W5V_MAL;   /* lo que ya se ve tiene que ser «W5V1» */
    if (k < W5V_CAB) return W5V_FALTA;
    c->an = (u16)le16(h + 4); c->al = (u16)le16(h + 6); c->fps = (u16)le16(h + 8); c->dur_ms = le32(h + 12);
    if (!c->an || !c->al || !c->fps) return W5V_MAL;
    w5v_tirar(a, W5V_CAB);
    return W5V_CUADRO;
}
int w5v_cuadro(const W5vAnillo *a, u32 desde, u32 *n, u32 *pts)
{
    u8 h[8];
    u32 k;
    if (w5v_mirar(a, desde, h, 8) < 8) return W5V_FALTA;
    k = le32(h); *pts = le32(h + 4); *n = k;
    if (k == 0 && *pts == W5V_FIN_PTS) return W5V_FIN;
    if (k < 4 || k > W5V_JPEG_MAX || k + 8 > a->cap) return W5V_MAL;
    if (a->n - desde < 8 + k) return W5V_FALTA;
    return W5V_CUADRO;
}
int w5v_elegir(W5vAnillo *a, int reloj, u32 tarde, u8 *jpg, u32 max, u32 *n, u32 *pts, u32 *saltados)
{
    for (;;) {
        u32 n0, p0, n1, p1;
        int r = w5v_cuadro(a, 0, &n0, &p0);
        if (r != W5V_CUADRO) return r;
        if (reloj >= 0) {
            u32 rl = (u32)reloj;
            if (p0 > rl) return W5V_FALTA;                                  /* aún no toca */
            if (rl - p0 > tarde || (w5v_cuadro(a, 8 + n0, &n1, &p1) == W5V_CUADRO && p1 <= rl)) {
                w5v_tirar(a, 8 + n0);                                        /* llegó tarde, o detrás hay otro que también toca: fuera sin decodificar */
                if (saltados) (*saltados)++;
                continue;
            }
        }
        if (n0 > max) return W5V_MAL;
        w5v_mirar(a, 8, jpg, n0);
        w5v_tirar(a, 8 + n0);
        *n = n0; *pts = p0;
        return W5V_CUADRO;
    }
}

/* ---- decodificar -------------------------------------------------------------------------------------------------------------------- */
/* img_jpeg pide su memoria con o->pedir (sin contexto): aquí sale de la arena de la app, de corrido, y se vuelve a empezar en cada cuadro.
 * Pide el trabajo, luego la salida, y suelta el trabajo: la salida queda en la arena hasta el cuadro siguiente. */
static W5vArena *arena_g;
static void *arena_pedir(size_t n)
{
    u32 k = ((u32)n + 7u) & ~7u;
    void *p;
    if (!arena_g || k > arena_g->cap - arena_g->usado) return NULL;
    p = arena_g->b + arena_g->usado;
    arena_g->usado += k;
    if (arena_g->usado > arena_g->pico) arena_g->pico = arena_g->usado;
    return p;
}
static void arena_soltar(void *p) { (void)p; }
int w5v_decodificar(W5vArena *ar, const u8 *jpg, u32 n, u16 max_w, u16 max_h, ImgSalida *s)
{
    ImgOpc o;
    int r;
    memset(&o, 0, sizeof o);
    o.pedir = arena_pedir; o.soltar = arena_soltar;
    o.max_w = max_w; o.max_h = max_h;
    o.max_mem = ar->cap > 64 ? ar->cap - 64 : ar->cap;                     /* lo que redondea la arena (8 por petición) cabe siempre */
    ar->usado = 0;
    arena_g = ar;
    r = img_jpeg(jpg, n, &o, s);
    arena_g = 0;
    return r;
}

/* ---- ampliar ------------------------------------------------------------------------------------------------------------------------ */
/* una fila × 1,5: de cada 2 píxeles, 3 (a a b). Con w múltiplo de 4, por palabras de 32 bits: 4 píxeles a b c e -> a a b c c e, es decir
 * las palabras (a|a<<16) (b|c<<16) (c|e<<16) = (p<<16 | p&0xFFFF) (p>>16 | q<<16) q   (little-endian, como el ARM del teléfono y el PC) */
static void fila_3_2(const u16 *o, int w, u16 *d)
{
    int x;
    if (!(w & 3) && !((size_t)o & 3) && !((size_t)d & 3)) {
        const u32 *s = (const u32 *)(const void *)o;
        u32 *t = (u32 *)(void *)d;
        for (x = 0; x < w; x += 4, s += 2, t += 3) {
            u32 p = s[0], q = s[1];
            t[0] = (p << 16) | (p & 0xFFFFu);
            t[1] = (p >> 16) | (q << 16);
            t[2] = q;
        }
        return;
    }
    for (x = 0; x + 1 < w; x += 2, d += 3) { u16 a = o[x], b = o[x + 1]; d[0] = a; d[1] = a; d[2] = b; }
}
void w5v_ampliar_3_2(const u16 *o, int w, int h, u16 *d)
{
    int W = w + (w >> 1), y;
    for (y = 0; y + 1 < h; y += 2) {                                       /* filas 3k, 3k+1, 3k+2 <- 2k, 2k, 2k+1 */
        u16 *d0 = d + (size_t)(y + (y >> 1)) * (size_t)W;
        fila_3_2(o + (size_t)y * (size_t)w, w, d0);
        memcpy(d0 + W, d0, (size_t)W * 2u);
        fila_3_2(o + (size_t)(y + 1) * (size_t)w, w, d0 + 2 * W);
    }
}

/* × 2 (de lado, 160×90 -> 320×180): cada píxel, un cuadrado de 2×2. La fila doblada por palabras de 32 bits (p | p<<16) y copiada debajo */
void w5v_ampliar_2(const u16 *o, int w, int h, u16 *d)
{
    int W = w * 2, x, y;
    for (y = 0; y < h; y++, o += w) {
        u16 *d0 = d + (size_t)(2 * y) * (size_t)W;
        if (!((size_t)d0 & 3)) { u32 *t = (u32 *)(void *)d0; for (x = 0; x < w; x++) { u32 p = o[x]; t[x] = p | (p << 16); } }
        else for (x = 0; x < w; x++) { d0[2 * x] = o[x]; d0[2 * x + 1] = o[x]; }
        memcpy(d0 + W, d0, (size_t)W * 2u);
    }
}

u32 w5v_dividir(u32 a, u32 b)                                              /* a / b sin el __aeabi_uidiv de libgcc (una vez por cuadro); b = 0: 0 */
{
    u32 q = 0, bit = 1;
    if (!b) return 0;
    while (b < a && !(b & 0x80000000u)) { b <<= 1; bit <<= 1; }
    while (bit) {
        if (a >= b) { a -= b; q |= bit; }
        b >>= 1; bit >>= 1;
    }
    return q;
}
void w5v_encajar(const u16 *o, int w, int h, u16 *d, int W, int H)
{
    int Wp, Hp, x0, y0, x, y, sx, sy, sy_ant = -1;
    u32 acc;
    u16 col[512];
    if (w <= 0 || h <= 0 || W <= 0 || H <= 0 || W > 512) return;
    if ((u32)w * (u32)H >= (u32)h * (u32)W) { Wp = W; Hp = (int)w5v_dividir((u32)h * (u32)W, (u32)w); }   /* más ancho: lleno a lo ancho */
    else { Hp = H; Wp = (int)w5v_dividir((u32)w * (u32)H, (u32)h); }
    if (Wp < 1) Wp = 1;
    if (Hp < 1) Hp = 1;
    x0 = (W - Wp) >> 1; y0 = (H - Hp) >> 1;
    for (x = 0, acc = 0, sx = 0; x < Wp; x++) {                           /* la columna de origen de cada una de destino: floor(x·w/Wp) */
        col[x] = (u16)sx;
        acc += (u32)w;
        while (acc >= (u32)Wp) { acc -= (u32)Wp; sx++; }
    }
    if (Hp < H || Wp < W) memset(d, 0, (size_t)W * (size_t)H * 2u);       /* las barras, negras */
    for (y = 0, acc = 0, sy = 0; y < Hp; y++) {
        u16 *t = d + (size_t)(y0 + y) * (size_t)W + x0;
        if (sy == sy_ant) memcpy(t, t - W, (size_t)Wp * 2u);              /* la misma fila de origen que la anterior */
        else {
            const u16 *s = o + (size_t)sy * (size_t)w;
            for (x = 0; x < Wp; x++) t[x] = s[col[x]];
        }
        sy_ant = sy;
        acc += (u32)h;
        while (acc >= (u32)Hp) { acc -= (u32)Hp; sy++; }
    }
}
