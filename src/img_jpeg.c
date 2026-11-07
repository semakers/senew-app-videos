/* img_jpeg.c — decodificador JPEG base propio de senew (contrato: img.h).
 *
 * Escrito a partir de la especificación (ITU T.81 y JFIF), sin código de terceros. Licencia MIT (como esta app).
 *
 * Qué decodifica:
 *  - SOF0 (base) y SOF1 (secuencial extendido de 8 bits con Huffman), un solo barrido entrelazado.
 *  - 1 componente (grises) o 3 (YCbCr; RGB directo si lo dice el marcador Adobe o los identificadores).
 *  - Factores de muestreo 1, 2 y 4 por componente: 4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1…
 *  - Tablas de cuantización de 8 y 16 bits, tablas Huffman de DHT, intervalos de reinicio con
 *    resincronización, relleno FF00, APPn/COM ignorados, EOI ausente tolerado.
 *  - Reducción 1/2, 1/4 y 1/8 en la IDCT (4x4, 2x2, 1x1).
 * Qué NO (IMG_E_NO_SOPORTADO): progresivo, aritmético, sin pérdida, jerárquico, 12 bits, 4 componentes
 *  (CMYK/YCCK), factores de muestreo 3, varios barridos (componentes no entrelazados), altura 0 (DNL).
 *
 * Cómo trabaja: por FILAS DE MCU. Cada componente tiene un búfer de una fila de MCU ya pasada por la
 * IDCT (a su tamaño reducido); al acabar la fila se sobremuestrea el croma, se convierte a RGB y se
 * escribe en la salida RGB565. Nunca existe la imagen entera en YCbCr.
 *
 * Aritmética: todo entero. Las cuentas que con datos corruptos podrían desbordar se hacen en u32
 * (el desbordamiento sin signo está definido) y se pasan a int al final. Se supone, como en todo el
 * proyecto (gcc), que int es de 32 bits en complemento a dos y que >> de un negativo es aritmético.
 * No hay divisiones (ni de 32 bits): no hace falta nada de libgcc.
 */
#include "img.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

#ifdef IMG_JPEG_PRUEBA
/* Solo para el arnés del PC: copia RGB888 antes de truncar a 565 y conmutador del sobremuestreo. */
extern u8 *img_jpeg_dbg_rgb;
extern int img_jpeg_dbg_caja;
#define CAJA_FORZADA img_jpeg_dbg_caja
#else
#define CAJA_FORZADA 0
#endif

/* ------------------------------------------------------------------------------------------------ */
/* Tablas constantes                                                                                */
/* ------------------------------------------------------------------------------------------------ */

/* Orden en zigzag: posición k del flujo -> índice fila*8+columna del bloque. */
static const u8 kZigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* IDCT reducida = IDCT de 8 puntos seguida de la media de cada 2 (o 4) muestras vecinas.
 * De la definición  f[m] = X0 + raíz(2)·Σ Xk·cos((2m+1)kπ/16):
 *   media de 2:  g[m] = X0 + raíz(2)·Σ Xk·cos(kπ/16)·cos((2m+1)kπ/8)        (el término k=4 se anula)
 *   media de 4:  h[m] = X0 + raíz(2)·Σ Xk·(1/4)·Σj cos((2(4m+j)+1)kπ/16)    (los k pares se anulan)
 * Coeficientes por 2^13. Filas = muestra de salida, columnas = frecuencia. */
static const short kRed4[4 * 8] = {
    8192,  10498,  7568,  3686, 0, -2463, -3135, -2088,
    8192,   4348, -7568, -8900, 0,  5946,  3135,  -865,
    8192,  -4348, -7568,  8900, 0, -5946,  3135,   865,
    8192, -10498,  7568, -3686, 0,  2463, -3135,  2088
};
static const short kRed2[2 * 8] = {
    8192,  7423, 0, -2607, 0,  1742, 0, -1477,
    8192, -7423, 0,  2607, 0, -1742, 0,  1477
};

/* ------------------------------------------------------------------------------------------------ */
/* Cabecera                                                                                         */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    u16 w, h;
    u8 nc;                    /* 1 o 3 */
    u8 sof;                   /* 1 si ya se vio un SOF válido */
    u8 id[3], hs[3], vs[3], tq[3], td[3], ta[3];
    const u8 *dqt[4];         /* 64 valores (8 o 16 bits) en zigzag */
    u8 dqt16[4];
    const u8 *dht[2][4];      /* [clase][id]: 16 cuentas y a continuación los símbolos */
    u16 dri;
    u8 jfif, adobe, adobe_tr;
    const u8 *datos;          /* primer byte de los datos del barrido */
} Cab;

static u32 be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }

/* Recorre los marcadores hasta el SOF (solo_sof) o hasta el SOS. Nunca lee fuera de [d, d+n). */
static int cabecera(const u8 *d, u32 n, Cab *c, int solo_sof)
{
    u32 pos = 2, len, sl, i;
    const u8 *seg;
    int m;

    memset(c, 0, sizeof *c);
    if (!d || n < 4 || d[0] != 0xFF || d[1] != 0xD8) return IMG_E_FORMATO;
    for (;;) {
        if (pos + 2 > n) return IMG_E_FORMATO;
        if (d[pos] != 0xFF) { pos++; continue; }            /* basura entre segmentos: se salta */
        m = d[pos + 1];
        if (m == 0xFF) { pos++; continue; }                 /* FF de relleno */
        pos += 2;
        if (m == 0x00 || m == 0x01 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) return IMG_E_FORMATO;                /* EOI sin imagen */
        if (pos + 2 > n) return IMG_E_FORMATO;
        len = be16(d + pos);
        if (len < 2 || len > n - pos) return IMG_E_FORMATO;
        seg = d + pos + 2;
        sl = len - 2;
        pos += len;

        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            /* SOFn */
            int nc;
            if (c->sof || sl < 6) return IMG_E_FORMATO;
            c->h = (u16)be16(seg + 1);
            c->w = (u16)be16(seg + 3);
            if (m != 0xC0 && m != 0xC1) return IMG_E_NO_SOPORTADO;   /* progresivo, sin pérdida, aritmético… */
            if (seg[0] != 8) return IMG_E_NO_SOPORTADO;              /* 12 bits */
            if (c->w == 0) return IMG_E_FORMATO;
            if (c->h == 0) return IMG_E_NO_SOPORTADO;                /* altura por DNL */
            nc = seg[5];
            if (nc != 1 && nc != 3) return nc == 0 ? IMG_E_FORMATO : IMG_E_NO_SOPORTADO;
            if (sl < 6 + 3 * (u32)nc) return IMG_E_FORMATO;
            for (i = 0; i < (u32)nc; i++) {
                const u8 *p = seg + 6 + 3 * i;
                int h = p[1] >> 4, v = p[1] & 15;
                if (h == 0 || v == 0 || h > 4 || v > 4 || p[2] > 3) return IMG_E_FORMATO;
                if (h == 3 || v == 3) return IMG_E_NO_SOPORTADO;
                c->id[i] = p[0]; c->hs[i] = (u8)h; c->vs[i] = (u8)v; c->tq[i] = p[2];
            }
            c->nc = (u8)nc;
            if (nc == 1) c->hs[0] = c->vs[0] = 1;           /* un solo componente: la MCU es un bloque */
            c->sof = 1;
            if (solo_sof) return IMG_OK;
        } else if (m == 0xC8) {
            return IMG_E_NO_SOPORTADO;                      /* extensiones JPEG */
        } else if (m == 0xDB) {                             /* DQT */
            while (sl > 0) {
                u32 pq = seg[0] >> 4, tq = seg[0] & 15, t = 1 + 64 * (pq + 1);
                if (pq > 1 || tq > 3 || sl < t) return IMG_E_FORMATO;
                c->dqt[tq] = seg + 1; c->dqt16[tq] = (u8)pq;
                seg += t; sl -= t;
            }
        } else if (m == 0xC4) {                             /* DHT */
            while (sl > 0) {
                u32 tc = seg[0] >> 4, th = seg[0] & 15, tot = 0;
                if (tc > 1 || th > 3 || sl < 17) return IMG_E_FORMATO;
                for (i = 1; i <= 16; i++) tot += seg[i];
                if (tot > 256 || sl < 17 + tot) return IMG_E_FORMATO;
                c->dht[tc][th] = seg + 1;
                seg += 17 + tot; sl -= 17 + tot;
            }
        } else if (m == 0xDD) {                             /* DRI */
            if (sl < 2) return IMG_E_FORMATO;
            c->dri = (u16)be16(seg);
        } else if (m == 0xE0) {                             /* APP0: JFIF */
            if (sl >= 5 && seg[0] == 'J' && seg[1] == 'F' && seg[2] == 'I' && seg[3] == 'F' && seg[4] == 0)
                c->jfif = 1;
        } else if (m == 0xEE) {                             /* APP14: Adobe */
            if (sl >= 12 && seg[0] == 'A' && seg[1] == 'd' && seg[2] == 'o' && seg[3] == 'b' && seg[4] == 'e') {
                c->adobe = 1; c->adobe_tr = seg[11];
            }
        } else if (m == 0xDA) {                             /* SOS */
            u32 ns;
            if (!c->sof || sl < 1) return IMG_E_FORMATO;
            ns = seg[0];
            if (ns == 0 || ns > 4 || sl < 1 + 2 * ns + 3) return IMG_E_FORMATO;
            if (ns != c->nc) return IMG_E_NO_SOPORTADO;     /* varios barridos */
            for (i = 0; i < ns; i++) {
                if (seg[1 + 2 * i] != c->id[i]) return IMG_E_NO_SOPORTADO;
                c->td[i] = seg[2 + 2 * i] >> 4; c->ta[i] = seg[2 + 2 * i] & 15;
                if (c->td[i] > 3 || c->ta[i] > 3) return IMG_E_FORMATO;
            }
            c->datos = d + pos;
            return IMG_OK;
        }
        /* cualquier otro segmento (APPn, COM, DAC…): ignorado */
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Lector de bits y Huffman                                                                         */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    const u8 *p, *fin;
    u32 acc;                  /* bits válidos arriba (el siguiente bit es el 31) */
    int nb;                   /* cuántos hay */
    int fant;                 /* de ellos, cuántos son ceros inventados (tras un marcador o el fin) */
} Lector;

/* Rellena hasta tener más de 24 bits. Ante un marcador o el final del fichero mete ceros «fantasma»
 * sin avanzar: quien decodifica comprueba después que no ha llegado a consumirlos. */
static void llenar(Lector *L)
{
    while (L->nb <= 24) {
        if (L->p < L->fin) {
            u32 b = *L->p;
            if (b != 0xFF) {
                L->p++;
            } else if (L->fin - L->p > 1 && L->p[1] == 0) {
                L->p += 2;                                  /* FF00 = un FF de datos */
            } else {
                L->nb += 8; L->fant += 8;                   /* marcador (o FF final): no se pasa */
                continue;
            }
            L->acc |= b << (24 - L->nb);
            L->nb += 8;
        } else {
            L->nb += 8; L->fant += 8;
        }
    }
}

#define BITS_RAPIDOS 9

typedef struct {
    u16 rapida[1 << BITS_RAPIDOS];   /* (longitud << 8) | símbolo para códigos de hasta 9 bits; 0 = no hay */
    int maxcode[17];                 /* mayor código de cada longitud, o -1 */
    int delta[17];                   /* índice del símbolo = código + delta[longitud] */
    int total;
    const u8 *vals;                  /* los símbolos, dentro del propio fichero */
} TablaH;

/* Construye la tabla canónica a partir de las 16 cuentas. -1 si las cuentas no forman un código. */
static int tabla_h(TablaH *t, const u8 *p)
{
    u32 code = 0, j;
    int l, idx = 0;

    memset(t->rapida, 0, sizeof t->rapida);
    t->vals = p + 16;
    t->maxcode[0] = -1; t->delta[0] = 0;
    for (l = 1; l <= 16; l++) {
        u32 nn = p[l - 1];
        if (code + nn > (1u << l)) return -1;
        t->maxcode[l] = nn ? (int)(code + nn - 1) : -1;
        t->delta[l] = idx - (int)code;
        if (l <= BITS_RAPIDOS) {
            for (j = 0; j < nn; j++) {
                u32 e = ((u32)l << 8) | t->vals[idx + (int)j];
                u32 k = (code + j) << (BITS_RAPIDOS - l), fin = k + (1u << (BITS_RAPIDOS - l));
                for (; k < fin; k++) t->rapida[k] = (u16)e;
            }
        }
        idx += (int)nn;
        code = (code + nn) << 1;
    }
    t->total = idx;
    return 0;
}

/* Un símbolo. -1 si el código no existe. */
static int simbolo(Lector *L, const TablaH *t)
{
    u32 e, c16, c;
    int l, idx;

    if (L->nb < 16) llenar(L);
    e = t->rapida[L->acc >> (32 - BITS_RAPIDOS)];
    if (e) {
        l = (int)(e >> 8);
        L->acc <<= l; L->nb -= l;
        return (int)(e & 255);
    }
    c16 = L->acc >> 16;
    for (l = BITS_RAPIDOS + 1; l <= 16; l++) {
        c = c16 >> (16 - l);
        if ((int)c <= t->maxcode[l]) {
            idx = (int)c + t->delta[l];
            if (idx < 0 || idx >= t->total) return -1;
            L->acc <<= l; L->nb -= l;
            return t->vals[idx];
        }
    }
    return -1;
}

/* s bits (1..15) con la extensión de signo de T.81 (F.2.2.1). */
static int recibir(Lector *L, int s)
{
    int v;
    if (L->nb < s) llenar(L);
    v = (int)(L->acc >> (32 - s));
    L->acc <<= s; L->nb -= s;
    if (v < (1 << (s - 1))) v -= (1 << s) - 1;
    return v;
}

/* Un bloque: DC diferencial + AC por series, ya multiplicado por la cuantización y colocado en orden
 * natural. Con solo_dc los AC se leen pero no se guardan (para la IDCT de 1x1).
 * Devuelve el último índice en zigzag escrito (0 = solo DC) o -1 si los datos no valen. */
static int bloque(Lector *L, const TablaH *dc, const TablaH *ac, const u16 *q, int *pred, int *c, int solo_dc)
{
    int s, k, r, ult = 0;

    s = simbolo(L, dc);
    if (s < 0 || s > 15) return -1;
    if (s) *pred = (int)((u32)*pred + (u32)recibir(L, s));
    c[0] = (int)((u32)*pred * q[0]);

    if (solo_dc) {
        for (k = 1; k < 64; ) {
            s = simbolo(L, ac);
            if (s < 0) return -1;
            r = s >> 4; s &= 15;
            if (s == 0) {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return -1;
            if (L->nb < s) llenar(L);
            L->acc <<= s; L->nb -= s;
            k++;
        }
    } else {
        memset(c + 1, 0, 63 * sizeof *c);
        for (k = 1; k < 64; ) {
            s = simbolo(L, ac);
            if (s < 0) return -1;
            r = s >> 4; s &= 15;
            if (s == 0) {
                if (r != 15) break;                         /* fin de bloque */
                k += 16;                                    /* 16 ceros */
                continue;
            }
            k += r;
            if (k > 63) return -1;
            c[kZigzag[k]] = (int)((u32)recibir(L, s) * q[k]);
            ult = k;
            k++;
        }
    }
    if (L->nb < L->fant) return -1;                         /* se consumieron bits que no existen */
    return ult;
}

/* ------------------------------------------------------------------------------------------------ */
/* IDCT                                                                                             */
/* ------------------------------------------------------------------------------------------------ */

/* Constantes raíz(2)·cos(kπ/16)·2^13 (k = 1, 2, 3, 5, 6, 7; la de k = 4 es exactamente 2^13). */
#define K1 11363u
#define K2 10703u
#define K3  9633u
#define K5  6436u
#define K6  4433u
#define K7  2260u

#define BITS_K   13           /* precisión de las constantes */
#define BITS_P1   2           /* bits de fracción que se conservan entre las dos pasadas */

/* Redondea y baja n bits un acumulador u32 que representa un entero con signo. */
#define BAJAR(x, n) ((int)((x) + (1u << ((n) - 1))) >> (n))

static u8 a_u8(int v)
{
    v += 128;
    if ((u32)v > 255) v = v < 0 ? 0 : 255;
    return (u8)v;
}

/* 1-D de 8 puntos, directa de la definición (escalada por raíz(8)):
 *   f[m] = X0 + raíz(2)·Σ k=1..7 Xk·cos((2m+1)kπ/16)
 * separada en parte par (k = 0, 2, 4, 6; simétrica) e impar (k = 1, 3, 5, 7; antisimétrica). */
#define IDCT_1D(x0, x1, x2, x3, x4, x5, x6, x7)                         \
    do {                                                                \
        u32 t0 = ((x0) + (x4)) << BITS_K, t1 = ((x0) - (x4)) << BITS_K; \
        u32 t2 = (x2) * K2 + (x6) * K6, t3 = (x2) * K6 - (x6) * K2;     \
        p0 = t0 + t2; p3 = t0 - t2; p1 = t1 + t3; p2 = t1 - t3;         \
        i0 = (x1) * K1 + (x3) * K3 + (x5) * K5 + (x7) * K7;             \
        i1 = (x1) * K3 - (x3) * K7 - (x5) * K1 - (x7) * K5;             \
        i2 = (x1) * K5 - (x3) * K1 + (x5) * K7 + (x7) * K3;             \
        i3 = (x1) * K7 - (x3) * K5 + (x5) * K3 - (x7) * K1;             \
    } while (0)

/* IDCT 8x8: columnas y luego filas. La salida total va dividida por 8 (2-D = 1-D·1-D / 8). */
static void idct8(const int *in, u8 *out, int paso)
{
    int ws[64], i;
    u32 p0, p1, p2, p3, i0, i1, i2, i3;

    for (i = 0; i < 8; i++) {
        u32 x0 = (u32)in[i], x1 = (u32)in[8 + i], x2 = (u32)in[16 + i], x3 = (u32)in[24 + i];
        u32 x4 = (u32)in[32 + i], x5 = (u32)in[40 + i], x6 = (u32)in[48 + i], x7 = (u32)in[56 + i];
        int *w = ws + i;
        if ((x1 | x2 | x3 | x4 | x5 | x6 | x7) == 0) {      /* columna con solo DC: constante */
            int v = (int)(x0 << BITS_P1);
            w[0] = v; w[8] = v; w[16] = v; w[24] = v; w[32] = v; w[40] = v; w[48] = v; w[56] = v;
            continue;
        }
        IDCT_1D(x0, x1, x2, x3, x4, x5, x6, x7);
        w[0]  = BAJAR(p0 + i0, BITS_K - BITS_P1); w[56] = BAJAR(p0 - i0, BITS_K - BITS_P1);
        w[8]  = BAJAR(p1 + i1, BITS_K - BITS_P1); w[48] = BAJAR(p1 - i1, BITS_K - BITS_P1);
        w[16] = BAJAR(p2 + i2, BITS_K - BITS_P1); w[40] = BAJAR(p2 - i2, BITS_K - BITS_P1);
        w[24] = BAJAR(p3 + i3, BITS_K - BITS_P1); w[32] = BAJAR(p3 - i3, BITS_K - BITS_P1);
    }
    for (i = 0; i < 8; i++, out += paso) {
        const int *w = ws + 8 * i;
        u32 x0 = (u32)w[0], x1 = (u32)w[1], x2 = (u32)w[2], x3 = (u32)w[3];
        u32 x4 = (u32)w[4], x5 = (u32)w[5], x6 = (u32)w[6], x7 = (u32)w[7];
        IDCT_1D(x0, x1, x2, x3, x4, x5, x6, x7);
        out[0] = a_u8(BAJAR(p0 + i0, BITS_K + BITS_P1 + 3)); out[7] = a_u8(BAJAR(p0 - i0, BITS_K + BITS_P1 + 3));
        out[1] = a_u8(BAJAR(p1 + i1, BITS_K + BITS_P1 + 3)); out[6] = a_u8(BAJAR(p1 - i1, BITS_K + BITS_P1 + 3));
        out[2] = a_u8(BAJAR(p2 + i2, BITS_K + BITS_P1 + 3)); out[5] = a_u8(BAJAR(p2 - i2, BITS_K + BITS_P1 + 3));
        out[3] = a_u8(BAJAR(p3 + i3, BITS_K + BITS_P1 + 3)); out[4] = a_u8(BAJAR(p3 - i3, BITS_K + BITS_P1 + 3));
    }
}

/* IDCT reducida a nn x nn (4 o 2) con la matriz kRed4/kRed2: columnas y luego filas. */
static void idct_red(const int *in, u8 *out, int paso, int nn, const short *mat)
{
    int ws[4 * 8], col, m, x, k;

    for (col = 0; col < 8; col++) {
        const int *ci = in + col;
        if ((ci[8] | ci[16] | ci[24] | ci[40] | ci[48] | ci[56]) == 0) {
            int v = (int)((u32)ci[0] << BITS_P1);
            for (m = 0; m < nn; m++) ws[m * 8 + col] = v;
            continue;
        }
        for (m = 0; m < nn; m++) {
            const short *f = mat + m * 8;
            u32 a = (u32)ci[0] << BITS_K;
            for (k = 1; k < 8; k++) a += (u32)ci[8 * k] * (u32)(int)f[k];
            ws[m * 8 + col] = BAJAR(a, BITS_K - BITS_P1);
        }
    }
    for (m = 0; m < nn; m++, out += paso) {
        const int *w = ws + m * 8;
        for (x = 0; x < nn; x++) {
            const short *f = mat + x * 8;
            u32 a = (u32)w[0] << BITS_K;
            for (k = 1; k < 8; k++) a += (u32)w[k] * (u32)(int)f[k];
            out[x] = a_u8(BAJAR(a, BITS_K + BITS_P1 + 3));
        }
    }
}

/* Del bloque de coeficientes a nn x nn muestras. ult = último índice en zigzag con dato. */
static void idct(const int *c, int ult, u8 *out, int paso, int nn)
{
    if (ult == 0 || nn == 1) {                              /* solo DC: bloque plano (DC/8) */
        u8 v = a_u8(BAJAR((u32)c[0], 3));
        int y;
        for (y = 0; y < nn; y++, out += paso) memset(out, v, (size_t)nn);
    } else if (nn == 8) {
        idct8(c, out, paso);
    } else if (nn == 4) {
        idct_red(c, out, paso, 4, kRed4);
    } else {
        idct_red(c, out, paso, 2, kRed2);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Geometría y estado                                                                               */
/* ------------------------------------------------------------------------------------------------ */

enum { SUB_NADA, SUB_CAJA, SUB_H2V1, SUB_H2V2, SUB_H1V2 };

typedef struct {
    u8 hs, vs;                /* bloques por MCU */
    u8 n;                     /* lado de su IDCT: 8, 4, 2 o 1 */
    u8 luh, luv;              /* log2 del sobremuestreo que le queda hasta el tamaño de salida */
    u8 modo;                  /* SUB_* */
    int paso;                 /* ancho de su búfer de fila de MCU */
    int filas;                /* y su alto */
    int cw, ch;               /* tamaño real del componente (sin el relleno de las MCU) */
} GeoC;

typedef struct {
    GeoC c[3];
    int nc, lf;               /* reducción = 1 << lf */
    int wo, ho;               /* salida */
    int mcux, mcuy, mw, mh;   /* MCU por fila y columna; tamaño de la MCU en píxeles de salida */
    int retraso;              /* 1 si la última fila de cada fila de MCU espera a la siguiente */
} Geo;

static int lg2(int v) { return v >> 1; }                    /* solo para 1, 2 y 4 */

/* Reparto de tamaños para una reducción dada. El croma submuestreado se «sobremuestrea» todo lo que
 * se pueda dentro de la propia IDCT (p. ej. 4:2:0 a 1/2: luma 4x4 y croma 8x8, ya del mismo tamaño). */
static void geometria(const Cab *c, int lf, Geo *g)
{
    int i, hmax = 1, vmax = 1, lhm, lvm;

    memset(g, 0, sizeof *g);
    for (i = 0; i < c->nc; i++) {
        if (c->hs[i] > hmax) hmax = c->hs[i];
        if (c->vs[i] > vmax) vmax = c->vs[i];
    }
    lhm = lg2(hmax); lvm = lg2(vmax);
    g->nc = c->nc; g->lf = lf;
    g->wo = (c->w + (1 << lf) - 1) >> lf;
    g->ho = (c->h + (1 << lf) - 1) >> lf;
    g->mcux = (c->w + 8 * hmax - 1) >> (3 + lhm);
    g->mcuy = (c->h + 8 * vmax - 1) >> (3 + lvm);
    g->mw = hmax * (8 >> lf);
    g->mh = vmax * (8 >> lf);
    for (i = 0; i < c->nc; i++) {
        GeoC *q = &g->c[i];
        int lrh = lhm - lg2(c->hs[i]), lrv = lvm - lg2(c->vs[i]), lk = lf;
        if (lk > lrh) lk = lrh;
        if (lk > lrv) lk = lrv;
        q->hs = c->hs[i]; q->vs = c->vs[i];
        q->n = (u8)((8 >> lf) << lk);
        q->luh = (u8)(lrh - lk); q->luv = (u8)(lrv - lk);
        q->paso = g->mcux * q->hs * q->n;
        q->filas = q->vs * q->n;
        q->cw = (g->wo + (1 << q->luh) - 1) >> q->luh;
        q->ch = (g->ho + (1 << q->luv) - 1) >> q->luv;
        if (q->luh == 0 && q->luv == 0) q->modo = SUB_NADA;
        else if (CAJA_FORZADA || lf == 3) q->modo = SUB_CAJA;
        else if (q->luh == 1 && q->luv == 0 && q->cw > 2) q->modo = SUB_H2V1;
        else if (q->luh == 1 && q->luv == 1 && q->cw > 2) q->modo = SUB_H2V2;
        else if (q->luh == 0 && q->luv == 1) q->modo = SUB_H1V2;
        else q->modo = SUB_CAJA;
        if (q->modo == SUB_H2V2 || q->modo == SUB_H1V2) g->retraso = 1;
    }
}

typedef struct {
    u8 *buf;                  /* fila de MCU ya en muestras: paso x filas */
    u8 *guarda;               /* última línea de la fila de MCU anterior (solo con retraso) */
    u8 *tmp;                  /* línea sobremuestreada */
    const TablaH *dc, *ac;
    int pred;
    u16 q[64];
} Comp;

typedef struct {
    Geo g;
    Comp c[3];
    Lector L;
    TablaH *tablas;
    u16 *pix;
    u8 *mal_act, *mal_ant;    /* MCU sin decodificar de la fila actual y de la anterior */
    int nmal_act, nmal_ant;
    int fila_mcu;             /* fila de MCU que hay en los búferes */
    int rgb;                  /* 3 componentes que ya son RGB */
    u8 fr, fg, fb;
    /* reinicios */
    u32 ri;                   /* MCU por intervalo (0 = sin reinicios) */
    u32 quedan;               /* MCU que faltan del intervalo */
    u32 intervalo;            /* índice del intervalo en curso */
    u32 reanudar;             /* primera MCU válida tras una resincronización */
    int parado;               /* ya no hay más datos aprovechables */
} Dec;

#define R4(x) (((u32)(x) + 3u) & ~3u)

/* Reparte el bloque de trabajo. Con base == NULL solo calcula su tamaño. */
static u32 reparto(const Geo *g, int ntab, u8 *base, Dec *D)
{
    u32 off = R4(sizeof(Dec)), t;
    int i;

    if (base) D->tablas = (TablaH *)(void *)(base + off);
    off += R4((u32)ntab * sizeof(TablaH));
    for (i = 0; i < g->nc; i++) {
        const GeoC *q = &g->c[i];
        t = (u32)q->paso * (u32)q->filas;
        if (base) D->c[i].buf = base + off;
        off += R4(t);
        if (base) D->c[i].guarda = g->retraso ? base + off : D->c[i].buf;
        if (g->retraso) off += R4(q->paso);
        if (base) D->c[i].tmp = q->modo != SUB_NADA ? base + off : D->c[i].buf;
        if (q->modo != SUB_NADA) off += R4(g->wo + 4);
    }
    if (base) { D->mal_act = base + off; D->mal_ant = base + off + g->mcux; }
    off += R4(2 * g->mcux);
    return off;
}

/* ------------------------------------------------------------------------------------------------ */
/* De los búferes de componentes a la salida                                                        */
/* ------------------------------------------------------------------------------------------------ */

/* Línea cr del componente i. Fuera de la imagen se repite el borde; la línea anterior a la fila de
 * MCU actual es la guardada; por debajo de lo que hay (solo al cortar por error) se repite la última. */
static const u8 *linea(const Dec *D, int i, int cr)
{
    const GeoC *q = &D->g.c[i];
    int k;
    if (cr > q->ch - 1) cr = q->ch - 1;
    if (cr < 0) cr = 0;
    k = cr - D->fila_mcu * q->filas;
    if (k < 0) return D->c[i].guarda;
    if (k >= q->filas) k = q->filas - 1;
    return D->c[i].buf + k * q->paso;
}

static u16 a565(int r, int g, int b)
{
    u32 v = ((u32)(r >> 3) << 11) | ((u32)(g >> 2) << 5) | (u32)(b >> 3);
    return (u16)((v >> 8) | (v << 8));
}

#ifdef IMG_JPEG_PRUEBA
#define PONER(o, idx, r, g, b)                                              \
    do {                                                                    \
        (o)[x] = a565((r), (g), (b));                                       \
        if (img_jpeg_dbg_rgb) {                                             \
            u8 *z_ = img_jpeg_dbg_rgb + 3 * (size_t)(idx);                  \
            z_[0] = (u8)(r); z_[1] = (u8)(g); z_[2] = (u8)(b);              \
        }                                                                   \
    } while (0)
#else
#define PONER(o, idx, r, g, b) ((o)[x] = a565((r), (g), (b)))
#endif

#define RECORTA(v) do { if ((u32)(v) > 255) (v) = (v) < 0 ? 0 : 255; } while (0)

/* Escribe la fila y de la salida. mal/nmal: MCU de esa fila que no se decodificaron (van al fondo). */
static void emitir(Dec *D, int y, const u8 *mal, int nmal)
{
    const Geo *g = &D->g;
    const u8 *pl[3];
    int i, x, w = g->wo;
    u16 *o = D->pix + (size_t)y * (size_t)w;
    size_t base = (size_t)y * (size_t)w;

    (void)base;
    for (i = 0; i < g->nc; i++) {
        const GeoC *q = &g->c[i];
        u8 *t = D->c[i].tmp;
        switch (q->modo) {
        case SUB_NADA:
            pl[i] = linea(D, i, y);
            continue;
        case SUB_CAJA: {                                    /* replicación */
            const u8 *s = linea(D, i, y >> q->luv);
            int sh = q->luh;
            for (x = 0; x < w; x++) t[x] = s[x >> sh];
            break;
        }
        case SUB_H2V1: {                                    /* triangular horizontal: 3/4 + 1/4 */
            const u8 *s = linea(D, i, y);
            int n = q->cw, a, p, c;
            p = c = s[0];
            for (x = 0; x < n; x++) {
                a = x + 1 < n ? s[x + 1] : c;
                t[2 * x] = (u8)((3 * c + p + 1) >> 2);
                t[2 * x + 1] = (u8)((3 * c + a + 2) >> 2);
                p = c; c = a;
            }
            break;
        }
        case SUB_H2V2: {                                    /* triangular en los dos ejes: 9, 3, 3, 1 /16 */
            int cr = y >> 1;
            const u8 *s = linea(D, i, cr), *v = linea(D, i, (y & 1) ? cr + 1 : cr - 1);
            int n = q->cw, a, p, c;
            p = c = 3 * s[0] + v[0];
            for (x = 0; x < n; x++) {
                a = x + 1 < n ? 3 * s[x + 1] + v[x + 1] : c;
                t[2 * x] = (u8)((3 * c + p + 8) >> 4);
                t[2 * x + 1] = (u8)((3 * c + a + 7) >> 4);
                p = c; c = a;
            }
            break;
        }
        default: {                                          /* SUB_H1V2: triangular vertical */
            int cr = y >> 1, sesgo = (y & 1) ? 2 : 1;
            const u8 *s = linea(D, i, cr), *v = linea(D, i, (y & 1) ? cr + 1 : cr - 1);
            for (x = 0; x < w; x++) t[x] = (u8)((3 * s[x] + v[x] + sesgo) >> 2);
            break;
        }
        }
        pl[i] = t;
    }

    if (g->nc == 1) {
        const u8 *py = pl[0];
        for (x = 0; x < w; x++) {
            int v = py[x];
            PONER(o, base + (size_t)x, v, v, v);
        }
    } else if (D->rgb) {
        for (x = 0; x < w; x++) {
            int r = pl[0][x], gg = pl[1][x], b = pl[2][x];
            PONER(o, base + (size_t)x, r, gg, b);
        }
    } else {
        /* JFIF: R = Y + 1.402 Cr,  G = Y - 0.34414 Cb - 0.71414 Cr,  B = Y + 1.772 Cb  (por 2^16) */
        const u8 *py = pl[0], *pb = pl[1], *pr = pl[2];
        for (x = 0; x < w; x++) {
            int yy = py[x], cb = pb[x] - 128, cr = pr[x] - 128;
            int r = yy + ((91881 * cr + 32768) >> 16);
            int gg = yy + ((-22554 * cb - 46802 * cr + 32768) >> 16);
            int b = yy + ((116130 * cb + 32768) >> 16);
            RECORTA(r); RECORTA(gg); RECORTA(b);
            PONER(o, base + (size_t)x, r, gg, b);
        }
    }

    if (nmal) {
        int mx, x1;
        for (mx = 0; mx < g->mcux; mx++) {
            if (!mal[mx]) continue;
            x = mx * g->mw;
            if (x >= w) break;
            x1 = x + g->mw;
            if (x1 > w) x1 = w;
            for (; x < x1; x++) PONER(o, base + (size_t)x, D->fr, D->fg, D->fb);
        }
    }
}

/* Tras decodificar la fila de MCU D->fila_mcu: saca sus filas de píxeles. Con sobremuestreo
 * triangular vertical, la última fila necesita el croma de la fila de MCU siguiente y se deja
 * pendiente (salvo que sea la última). */
static void sacar_fila_mcu(Dec *D, int ultima)
{
    const Geo *g = &D->g;
    int y0 = D->fila_mcu * g->mh, y, i;

    if (g->retraso && D->fila_mcu > 0 && y0 - 1 < g->ho)
        emitir(D, y0 - 1, D->mal_ant, D->nmal_ant);
    for (y = y0; y < y0 + g->mh && y < g->ho; y++) {
        if (g->retraso && !ultima && y == y0 + g->mh - 1) break;
        emitir(D, y, D->mal_act, D->nmal_act);
    }
    if (g->retraso)
        for (i = 0; i < g->nc; i++)
            memcpy(D->c[i].guarda, D->c[i].buf + (g->c[i].filas - 1) * g->c[i].paso, (size_t)g->c[i].paso);
}

/* ------------------------------------------------------------------------------------------------ */
/* Barrido                                                                                          */
/* ------------------------------------------------------------------------------------------------ */

static void reiniciar_lector(Dec *D, const u8 *p)
{
    int i;
    D->L.p = p; D->L.acc = 0; D->L.nb = 0; D->L.fant = 0;
    for (i = 0; i < D->g.nc; i++) D->c[i].pred = 0;
}

/* En la frontera de un intervalo: ¿está justo aquí el RSTn esperado? */
static int leer_rst(Dec *D)
{
    const u8 *p = D->L.p, *fin = D->L.fin;
    while (fin - p > 2 && p[0] == 0xFF && p[1] == 0xFF) p++;
    if (fin - p >= 2 && p[0] == 0xFF && p[1] == (0xD0 | (D->intervalo & 7))) {
        D->intervalo++;
        D->quedan = D->ri;
        reiniciar_lector(D, p + 2);
        return 1;
    }
    return 0;
}

/* Tras un error: busca el siguiente RSTn y calcula en qué MCU se reanuda. Si no hay reinicios o no
 * aparece ninguno, se acabó. El número del marcador dice cuántos intervalos se han perdido (hasta 7). */
static void resincronizar(Dec *D)
{
    const u8 *p = D->L.p, *fin = D->L.fin;

    if (D->ri == 0) { D->parado = 1; return; }
    while (fin - p > 1) {
        if (p[0] != 0xFF) { p++; continue; }
        if (p[1] == 0x00) { p += 2; continue; }
        if (p[1] == 0xFF) { p++; continue; }
        if (p[1] >= 0xD0 && p[1] <= 0xD7) {
            u32 k = D->intervalo + ((p[1] - D->intervalo) & 7);
            D->reanudar = (k + 1) * D->ri;
            D->intervalo = k + 1;
            D->quedan = D->ri;
            reiniciar_lector(D, p + 2);
            return;
        }
        break;                                              /* otro marcador (EOI…): no hay más */
    }
    D->parado = 1;
}

/* Una MCU: todos sus bloques, cada uno a su sitio del búfer de su componente. 0 si falla. */
static int mcu(Dec *D, int mx)
{
    int c[64], i, bx, by, ult;

    for (i = 0; i < D->g.nc; i++) {
        const GeoC *q = &D->g.c[i];
        Comp *k = &D->c[i];
        int n = q->n;
        for (by = 0; by < q->vs; by++) {
            for (bx = 0; bx < q->hs; bx++) {
                ult = bloque(&D->L, k->dc, k->ac, k->q, &k->pred, c, n == 1);
                if (ult < 0) return 0;
                idct(c, ult, k->buf + by * n * q->paso + (mx * q->hs + bx) * n, q->paso, n);
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------ */
/* Interfaz                                                                                         */
/* ------------------------------------------------------------------------------------------------ */

static void limpiar_salida(ImgSalida *s)
{
    s->w = s->h = s->w_orig = s->h_orig = 0;
    s->reduccion = 1; s->alfa = 0; s->mem_pico = 0; s->pix = NULL;
}

int img_jpeg_info(const u8 *d, u32 n, ImgSalida *s)
{
    Cab c;
    int rc;

    if (!s) return IMG_E_FORMATO;
    limpiar_salida(s);
    rc = cabecera(d, n, &c, 1);
    s->w_orig = c.w; s->h_orig = c.h;                       /* también si es una variante no soportada */
    return rc;
}

int img_jpeg(const u8 *d, u32 n, const ImgOpc *o, ImgSalida *s)
{
    Cab c;
    Geo g;
    Dec *D;
    u8 *trabajo;
    u16 *pix;
    u32 mem_trabajo = 0, mem_pix = 0, gi, buenos = 0, malos = 0, t0 = 0;
    int rc, i, j, lf, ntab, my, mx, sum, y;
    int idc[3], iac[3];

    if (!s) return IMG_E_FORMATO;
    limpiar_salida(s);
    if (!o || !o->pedir || !o->soltar) return IMG_E_MEMORIA;
    rc = cabecera(d, n, &c, 0);
    s->w_orig = c.w; s->h_orig = c.h;
    if (rc != IMG_OK) return rc;

    /* tablas que usa el barrido */
    sum = 0;
    for (i = 0; i < c.nc; i++) {
        if (!c.dqt[c.tq[i]] || !c.dht[0][c.td[i]] || !c.dht[1][c.ta[i]]) return IMG_E_DATOS;
        sum += c.hs[i] * c.vs[i];
    }
    if (sum > 10) return IMG_E_FORMATO;                     /* T.81 B.2.3: como mucho 10 bloques por MCU */

    /* tablas Huffman distintas (los componentes suelen compartirlas) */
    ntab = 0;
    for (i = 0; i < c.nc; i++) {
        idc[i] = -1; iac[i] = -1;
        for (j = 0; j < i; j++) if (c.td[j] == c.td[i]) idc[i] = idc[j];
        if (idc[i] < 0) idc[i] = ntab++;
    }
    for (i = 0; i < c.nc; i++) {
        for (j = 0; j < i; j++) if (c.ta[j] == c.ta[i]) iac[i] = iac[j];
        if (iac[i] < 0) iac[i] = ntab++;
    }

    /* la menor reducción que quepa en max_w x max_h y en max_mem */
    for (lf = 0; lf < 4; lf++) {
        u32 px;
        geometria(&c, lf, &g);
        if (o->max_w && g.wo > o->max_w) continue;
        if (o->max_h && g.ho > o->max_h) continue;
        px = (u32)g.wo * (u32)g.ho;
        if (px > 0x3FFFFFFFu) continue;
        mem_pix = px * 2;
        mem_trabajo = reparto(&g, ntab, NULL, NULL);
        if (o->max_mem && (mem_pix > o->max_mem || mem_trabajo > o->max_mem - mem_pix)) continue;
        break;
    }
    if (lf == 4) return IMG_E_GRANDE;

    trabajo = (u8 *)o->pedir(mem_trabajo);
    if (!trabajo) return IMG_E_MEMORIA;
    pix = (u16 *)o->pedir(mem_pix);
    if (!pix) { o->soltar(trabajo); return IMG_E_MEMORIA; }

    D = (Dec *)(void *)trabajo;
    memset(D, 0, sizeof *D);
    D->g = g;
    reparto(&g, ntab, trabajo, D);
    D->pix = pix;
    D->fr = o->fondo_r; D->fg = o->fondo_g; D->fb = o->fondo_b;
    for (i = 0; i < c.nc; i++) {
        const u8 *q = c.dqt[c.tq[i]];
        Comp *k = &D->c[i];
        for (j = 0; j < 64; j++) k->q[j] = c.dqt16[c.tq[i]] ? (u16)be16(q + 2 * j) : q[j];
        k->dc = &D->tablas[idc[i]]; k->ac = &D->tablas[iac[i]];
        if (tabla_h(&D->tablas[idc[i]], c.dht[0][c.td[i]]) || tabla_h(&D->tablas[iac[i]], c.dht[1][c.ta[i]])) {
            o->soltar(pix); o->soltar(trabajo);
            return IMG_E_DATOS;
        }
        memset(k->buf, 128, (size_t)g.c[i].paso * (size_t)g.c[i].filas);
        if (g.retraso) memset(k->guarda, 128, (size_t)g.c[i].paso);
    }
    memset(D->mal_act, 0, 2 * (size_t)g.mcux);
    if (c.nc == 3) {
        /* YCbCr salvo que el fichero diga otra cosa: JFIF manda; si no, Adobe; si no, los identificadores */
        if (c.jfif) D->rgb = 0;
        else if (c.adobe) D->rgb = c.adobe_tr == 0;
        else D->rgb = c.id[0] == 'R' && c.id[1] == 'G' && c.id[2] == 'B';
    }
    D->L.fin = d + n;
    reiniciar_lector(D, c.datos);
    D->ri = c.dri;
    D->quedan = c.dri;

    if (o->reloj && o->max_ms) t0 = o->reloj();
    gi = 0;
    y = 0;
    for (my = 0; my < g.mcuy; my++) {
        u8 *t;
        if (o->reloj && o->max_ms && (u32)(o->reloj() - t0) > o->max_ms) {
            o->soltar(pix); o->soltar(trabajo);
            return IMG_E_TIEMPO;
        }
        D->fila_mcu = my;
        t = D->mal_ant; D->mal_ant = D->mal_act; D->mal_act = t;
        D->nmal_ant = D->nmal_act; D->nmal_act = 0;
        memset(D->mal_act, 0, (size_t)g.mcux);

        for (mx = 0; mx < g.mcux; mx++, gi++) {
            if (!D->parado && gi >= D->reanudar) {
                if (D->ri && D->quedan == 0 && !leer_rst(D)) resincronizar(D);
                if (!D->parado && gi >= D->reanudar) {
                    if (mcu(D, mx)) {
                        buenos++;
                        D->quedan--;                        /* solo cuenta si hay reinicios */
                        continue;
                    }
                    resincronizar(D);
                }
            }
            D->mal_act[mx] = 1; D->nmal_act++; malos++;
        }
        sacar_fila_mcu(D, D->parado || my == g.mcuy - 1);
        y = (my + 1) * g.mh;
        if (D->parado) break;
    }
    if (y > g.ho) y = g.ho;

    if (buenos == 0) {
        o->soltar(pix); o->soltar(trabajo);
        return IMG_E_DATOS;
    }
    /* lo que no se llegó a decodificar: color de fondo */
    if (y < g.ho) {
        u16 f = a565(D->fr, D->fg, D->fb);
        u32 k, k0 = (u32)y * (u32)g.wo, k1 = (u32)g.ho * (u32)g.wo;
        for (k = k0; k < k1; k++) pix[k] = f;
#ifdef IMG_JPEG_PRUEBA
        if (img_jpeg_dbg_rgb)
            for (k = k0; k < k1; k++) {
                img_jpeg_dbg_rgb[3 * (size_t)k] = D->fr;
                img_jpeg_dbg_rgb[3 * (size_t)k + 1] = D->fg;
                img_jpeg_dbg_rgb[3 * (size_t)k + 2] = D->fb;
            }
#endif
        malos++;
    }
    o->soltar(trabajo);

    s->w = (u16)g.wo; s->h = (u16)g.ho;
    s->reduccion = (u8)(1 << lf);
    s->mem_pico = mem_trabajo + mem_pix;
    s->pix = pix;
    return malos ? IMG_PARCIAL : IMG_OK;
}
