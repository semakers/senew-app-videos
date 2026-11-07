/* w5v.h — el flujo .w5v de la app «Videos» (contrato en el repo de SENUS: docs/contrato_videos.md): el ANILLO donde cae lo que llega de la red, cortar de
 * él los cuadros, ELEGIR el que toca con el reloj (saltando sin decodificar los que van tarde), DECODIFICAR el JPEG con el decodificador
 * pequeño (img_jpeg.c, compilado dentro de la app) y AMPLIAR la salida al tamaño de la pantalla.
 *
 * C «freestanding», sin la API w5 ni libc (solo memcpy/memset), sin coma flotante y SIN DIVISIONES: lo mismo corre en la app (videos.c)
 * y en el arnés del PC (prueba/prueba_w5v.c), que pasa un .w5v por ESTE código y compara cada cuadro con ffmpeg.
 *
 * El cuerpo, little-endian:  'W5V1' u16 ancho u16 alto u16 fps u16 0 u32 duración_ms        (16 bytes, una vez)
 *                            u32 n  u32 pts_ms  n bytes de JPEG                              (cada cuadro)
 *                            u32 0  u32 0xFFFFFFFF                                           (fin; también vale cerrar) */
#ifndef W5V_H
#define W5V_H
#include "img.h"

#define W5V_CAB 16
#define W5V_JPEG_MAX (32u * 1024u)             /* un cuadro no pasa de 32 KB (contrato) */
#define W5V_FIN_PTS 0xFFFFFFFFu

/* ---- el anillo: cap potencia de 2; lo escribe quien recibe, lo lee quien corta ---- */
typedef struct {
    u8 *b;
    u32 cap, mask;
    u32 ini, n;                                /* el primer byte sin leer y cuántos hay */
    u32 entrados, salidos;                     /* bytes que han entrado y salido desde el principio (diagnóstico) */
} W5vAnillo;
void w5v_anillo(W5vAnillo *a, u8 *b, u32 cap);
u32 w5v_hueco(const W5vAnillo *a, u8 **d);     /* dónde escribir y cuántos bytes SEGUIDOS caben ahí (0 = lleno) */
void w5v_escrito(W5vAnillo *a, u32 k);         /* se escribieron k bytes en el hueco */
u32 w5v_mirar(const W5vAnillo *a, u32 desde, void *d, u32 k);   /* copia k bytes a partir de «desde» sin sacarlos; los que había */
void w5v_tirar(W5vAnillo *a, u32 k);

/* ---- la cabecera y los cuadros ---- */
typedef struct { u16 an, al, fps; u32 dur_ms; } W5vCab;
enum { W5V_MAL = -1, W5V_FALTA = 0, W5V_CUADRO = 1, W5V_FIN = 2 };
int w5v_cabecera(W5vAnillo *a, W5vCab *c);     /* W5V_CUADRO (leída y sacada) / W5V_FALTA / W5V_MAL (no es un .w5v) */
/* el cuadro que empieza «desde» bytes después del punto de lectura: W5V_CUADRO si está ENTERO en el anillo (n y pts), W5V_FALTA si aún
 * no, W5V_FIN si es la marca de fin, W5V_MAL si su tamaño no vale (0, > W5V_JPEG_MAX o no cabe en el anillo) */
int w5v_cuadro(const W5vAnillo *a, u32 desde, u32 *n, u32 *pts);

/* Elige el cuadro que toca. reloj: ms del video; < 0 = aún no corre (se da el PRIMERO que haya, para enseñarlo quieto). Saca del anillo SIN
 * decodificar los que ya pasaron: los que van más de «tarde» ms detrás del reloj y los que tienen detrás otro que también toca. El que
 * toca se COPIA a jpg (max bytes) y se saca. W5V_CUADRO (en *n, *pts) / W5V_FALTA (nada que enseñar ahora) / W5V_FIN / W5V_MAL. */
int w5v_elegir(W5vAnillo *a, int reloj, u32 tarde, u8 *jpg, u32 max, u32 *n, u32 *pts, u32 *saltados);

/* ---- decodificar: el JPEG a RGB565 (byte alto primero) en un bloque de trabajo de la app (sin montón del sistema) ---- */
typedef struct { u8 *b; u32 cap, usado, pico; } W5vArena;
/* max_w/max_h: si el cuadro es más grande, el decodificador lo reduce (1/2, 1/4, 1/8). s->pix vive en la arena hasta la próxima llamada.
 * Devuelve lo de img_jpeg: IMG_OK / IMG_PARCIAL (vale) o IMG_E_* */
int w5v_decodificar(W5vArena *ar, const u8 *jpg, u32 n, u16 max_w, u16 max_h, ImgSalida *s);

/* ---- ampliar ---- */
/* × 1,5 por el vecino más cercano (como w5v_encajar con W = 3w/2): w y h pares */
void w5v_ampliar_3_2(const u16 *o, int w, int h, u16 *d);
/* × 2 por el vecino más cercano (de lado): d es de 2w × 2h */
void w5v_ampliar_2(const u16 *o, int w, int h, u16 *d);
/* w×h encajado en W×H sin deformar (el vecino de abajo a la izquierda: sx = floor(x·w/W')), centrado, con barras negras. Sin divisiones */
void w5v_encajar(const u16 *o, int w, int h, u16 *d, int W, int H);
u32 w5v_dividir(u32 a, u32 b);                 /* a / b sin libgcc (b = 0: 0) */
#endif
