/* img.h — decodificadores PROPIOS de imágenes fijas (PNG y JPEG base) de senew.
 *
 * Contrato común (lo usan ui/ y las apps; cada decodificador vive en su fichero: img_png.c, img_jpeg.c):
 *  - C99 «freestanding»: sin libc (solo memcpy/memset/memcmp/memmove), sin coma flotante, sin división
 *    de 64 bits, sin recursión profunda, sin variables estáticas grandes (toda la memoria de trabajo se
 *    pide con o->pedir y se devuelve con o->soltar antes de volver, salvo s->pix).
 *  - La entrada es el fichero ENTERO en memoria (d, n). No se modifica.
 *  - La salida es RGB565 «del panel»: cada píxel es un u16 con el BYTE ALTO PRIMERO en memoria, es decir
 *    v = (r>>3)<<11 | (g>>2)<<5 | (b>>3)  y se guarda  (u16)((v >> 8) | (v << 8))   (igual que ui/ui_gif.c).
 *    La conversión de 8 bits a 5/6/5 es por TRUNCAMIENTO (>>3, >>2, >>3), sin tramado.
 *  - Reducción: si la imagen no cabe en max_w x max_h o su memoria pasaría de max_mem, se decodifica
 *    reducida por 2, 4 u 8 (la menor que quepa). JPEG: reduciendo en la IDCT (4x4, 2x2, 1x1). PNG: media
 *    de cada bloque de f x f píxeles (tras mezclar el alfa). Si ni con 8 cabe: IMG_E_GRANDE.
 *    Tamaño de salida = techo(orig / f).
 *  - Alfa (PNG): se mezcla sobre el color de fondo dado (8 bits por canal):
 *    c = (a * c + (255 - a) * fondo + 127) / 255   (16 bits: se usa el byte alto de cada muestra).
 *  - Datos corruptos o truncados: NUNCA leer fuera de [d, d+n) ni escribir fuera de lo pedido; devolver
 *    un error (o la parte decodificada con IMG_PARCIAL si ya había al menos una fila/MCU: el resto de
 *    píxeles queda con el color de fondo).
 *  - Tope de tiempo: si o->reloj no es NULL y o->max_ms > 0, se consulta cada pocas filas/MCU y se
 *    abandona con IMG_E_TIEMPO si se pasa.
 */
#ifndef IMG_H
#define IMG_H
#include <stddef.h>
#ifndef HW_H
#ifndef HAL_H
#ifndef UI_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
#endif
#endif
#endif

enum {
    IMG_OK = 0,
    IMG_PARCIAL = 1,          /* se decodificó una parte (fichero truncado o dañado a mitad); s->pix es válido */
    IMG_E_FORMATO = -1,       /* no es de este formato o la cabecera está mal */
    IMG_E_NO_SOPORTADO = -2,  /* variante que no se decodifica (JPEG progresivo/aritmético/12 bits, PNG entrelazado si no se hace…) */
    IMG_E_GRANDE = -3,        /* no cabe en max_w x max_h / max_mem ni reducida a 1/8 */
    IMG_E_MEMORIA = -4,       /* o->pedir devolvió NULL */
    IMG_E_DATOS = -5,         /* datos corruptos antes de poder pintar nada */
    IMG_E_TIEMPO = -6         /* se pasó de max_ms */
};

typedef struct {
    void *(*pedir)(size_t n);         /* puede devolver NULL */
    void  (*soltar)(void *p);
    u16 max_w, max_h;                 /* tamaño máximo de la salida (p. ej. 176 x 220) */
    u32 max_mem;                      /* tope de bytes pedidos a la vez, salida incluida (0 = sin tope) */
    u8  fondo_r, fondo_g, fondo_b;    /* fondo para el alfa y para lo que quede sin decodificar */
    u32 (*reloj)(void);               /* milisegundos; NULL = sin tope de tiempo */
    u32 max_ms;
} ImgOpc;

typedef struct {
    u16 w, h;                         /* tamaño de la salida */
    u16 w_orig, h_orig;               /* tamaño del fichero */
    u8  reduccion;                    /* 1, 2, 4 u 8 */
    u8  alfa;                         /* 1 si el fichero traía transparencia (ya mezclada) */
    u32 mem_pico;                     /* pico de bytes pedidos (salida incluida) */
    u16 *pix;                         /* w*h píxeles, pedidos con o->pedir: los libera QUIEN LLAMA con o->soltar */
} ImgSalida;

/* Solo la cabecera: rellena w_orig/h_orig (y alfa). IMG_OK o IMG_E_*. No pide memoria. */
int img_png_info(const u8 *d, u32 n, ImgSalida *s);
int img_jpeg_info(const u8 *d, u32 n, ImgSalida *s);
/* Decodifican. Devuelven IMG_OK / IMG_PARCIAL (s->pix válido) o IMG_E_* (s->pix = NULL, nada pendiente de liberar). */
int img_png(const u8 *d, u32 n, const ImgOpc *o, ImgSalida *s);
int img_jpeg(const u8 *d, u32 n, const ImgOpc *o, ImgSalida *s);
#endif
