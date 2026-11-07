/* videos.c — «Videos»: buscar videos y VERLOS en el W580, con la red Bluetooth de SENUS. El servicio (SENUS, puerto 8580) devuelve la lista
 * de resultados, el sonido (un MP3) y el video como un flujo de cuadros JPEG con su tiempo (formato «.w5v»).
 *
 * Como Música (senew-app-musica): buscar -> lista -> OK. Al ver, a la vez:
 *   - el SONIDO lo toca el sistema por su cola de la red (w5_musica_red.h): «/pista/<id>.mp3»;
 *   - el VIDEO lo baja la app por un canal de la red (w5_red.h): «/videos/<id>.w5v», cuadros JPEG con su tiempo, a un ANILLO de 64 KB (w5v.c).
 * El reloj del video es mu_red_info().pos_ms mientras suena (antes, el primer cuadro quieto). En cada vuelta se elige el cuadro que toca: los
 * que llegan tarde (> 150 ms detrás del reloj) se saltan SIN decodificar; el que toca se decodifica con el decodificador JPEG pequeño de
 * esta carpeta (img_jpeg.c, compilado DENTRO de la app), se amplía × 1,5 (160×90 -> 240×135) y se pinta con li_pixeles.
 * DE LADO (W5_CAP_GIRA: el acelerómetro gira la pantalla a 320×240) el reproductor es a pantalla completa: el cuadro × 2 (320×180), el título
 * arriba y el tiempo abajo, sin softkeys (OK y Atrás valen igual). Al girar, el último cuadro se vuelve a ampliar al tamaño nuevo.
 * Si el sonido no llega (falla, o no avanza en 15 s), el video sigue con un reloj propio, sin sonido, y lo dice.
 *
 *   LISTA   OK  ver            sel. izq.  Buscar            sel. der. / Atrás  salir
 *   VER     OK / sel. izq.  pausa o seguir (mu_alternar)    sel. der. / Atrás  parar y volver a la lista      #  medir (al registro)
 * Al salir de un video: mu_parar y red_cerrar de su canal. Al salir de la app, también. */
#include "w5.h"
#include "w5_pantalla.h"
#include "w5_red.h"
#include "w5_musica_red.h"
#include "w5v.h"

/* la pila: el decodificador corre en ella (img_jpeg: ~1,5 KB medido en el PC; aquí, W5_Q_PILA_LIBRE al registro); el montón: anillo 64 KB +
 * JPEG 32 KB + arena del decodificador 48 KB (salida 160×90 = 28 800 B y su trabajo, ~10 KB) + ampliado al mayor, 320×180 = 115 200 B
 * + miniatura 7 200 B + el campo de texto de Buscar */
W5_APP(.nombre = "Videos", .icono_gen = W5_IC_IMAGENES, .version = 3, .api_menor = W5_MURED_MENOR, .caps = W5_CAP_MUSICA | W5_CAP_GIRA,
       .pila = 16384, .monton = 320 * 1024);
/* sin W5_CAP_RED (como Música): carga también donde no hay red, y lo dice */

#ifndef VID_PUERTO
#define VID_PUERTO 8580                         /* el servicio de SENUS; el puente lo deja pasar. Otro, solo
                                                 * para el emulador (prueba/emu_videos.py: -DVID_PUERTO=18580, el servicio de mentira) */
#endif
#define VID_TXT_(x) #x
#define VID_TXT(x) VID_TXT_(x)
#define VID_SERV "http://127.0.0.1:" VID_TXT(VID_PUERTO)
#define AN 160                                  /* el cuadro que se pide (contrato) */
#define AL 90
#define FPS 12
#define PAN_AN 240                              /* el ampliado de pie: × 1,5 */
#define PAN_AL 135
#define LADO_AN 320                             /* de lado: × 2, a todo lo ancho */
#define LADO_AL 180
#define TARDE_MS 150                            /* un cuadro más de esto detrás del reloj se salta sin decodificar (contrato) */
#ifndef VID_ESPERA_SONIDO_MS
#define VID_ESPERA_SONIDO_MS 15000              /* el sonido sin empezar y sin bajar nada en este tiempo: el video sigue sin él */
#endif
#define ANILLO (64u * 1024u)
#define ARENA (48u * 1024u)
#define MINI_AN 80
#define MINI_AL 45
#define MAXR 40

typedef struct { char id[16]; unsigned dur_s; char titulo[W5_MURED_TITULO]; char canal[W5_MURED_ARTISTA]; } Res;
static Res res[MAXR];
static int n_res, hay_mas, sel;
static char consulta[48], encabezado[64];
enum { P_NADA, P_BUSCAR, P_MAS, P_MINI };
static struct {                                  /* las peticiones de la lista (de una en una, como Música) */
    int red_ok, pide, canal, http;
    unsigned n;
    char aviso[112], url[176];
    char mini_id[16], mini_pide[16];
    int mini_ok, mini_version;
} s;
static unsigned char cuerpo[8192];
static W5Ctx yo;

/* la memoria grande, del montón de la app (una vez: no se suelta hasta descargar) */
static u8 *anillo, *jpg, *arena_b;
static u16 *ampl, *mini;
static W5vAnillo an;
static W5vArena arena;

/* el reproductor */
enum { R_CONECTANDO, R_PIDIENDO, R_LEYENDO, R_ACABADO, R_FALLO };
static struct {
    int activo, i, fase, canal, http, reintentos;
    char url[176], aviso[96];
    int cab_ok, mostrado, fin;                   /* fin: llegó la marca de fin (o se cerró) y no quedan cuadros */
    W5vCab cab;
    u32 pts, desde_ms, jpg_n;                    /* el último cuadro enseñado; desde dónde se pidió; los bytes de su JPEG (en jpg) */
    int img_w, img_h, version;
    int amp_w, amp_h;                            /* lo que tiene ampl ahora (de pie 240×135, de lado 320×180) */
    /* el reloj */
    int sonido, sonido_visto, sonido_empezo, propio, propio_pausa;   /* sonido_visto: la cola de la red se puso en marcha */
    u32 propio_t0, propio_pts0, propio_parado;
    u32 rec_ult, t_rec_ult;
    char sin_sonido[48];
    /* lo medido */
    u32 dec_n, dec_ms, amp_ms, pin_n, pin_ms, saltados, errores, t_primero, n_primero;
    u32 seg_t, seg_n, fps10, t_log, pila_min;
    int med_fase, med_cuenta; u32 med_t0, med_dec, med_amp, med_pin;
} v;

#define DIV(a, b) w5v_dividir((a), (b))           /* sin __aeabi_uidiv: la .sen no tira de las divisiones de libgcc */
static void copiar(char *d, const char *o, unsigned max) { unsigned n = (unsigned)strlen(o); if (n >= max) n = max - 1; memcpy(d, o, n); d[n] = 0; }
static const char *mmss(unsigned ms) { return ui_fmt("%u:%02u", ms / 60000u, ms / 1000u % 60u); }
static void avisar(const char *t) { copiar(s.aviso, t, sizeof s.aviso); }
static void urlcod(char *d, unsigned max, const char *t)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned n = 0;
    for (; *t && n + 4 < max; t++) {
        unsigned char c = (unsigned char)*t;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') d[n++] = (char)c;
        else if (c == ' ') d[n++] = '+';
        else { d[n++] = '%'; d[n++] = hex[c >> 4]; d[n++] = hex[c & 15]; }
    }
    d[n] = 0;
}
static int memoria(void)                         /* los búferes grandes, la primera vez */
{
    if (anillo) return 1;
    anillo = (u8 *)ap_mem_pedir(ANILLO); jpg = (u8 *)ap_mem_pedir(W5V_JPEG_MAX); arena_b = (u8 *)ap_mem_pedir(ARENA);
    ampl = (u16 *)ap_mem_pedir(LADO_AN * LADO_AL * 2); mini = (u16 *)ap_mem_pedir(MINI_AN * MINI_AL * 2);
    if (!anillo || !jpg || !arena_b || !ampl || !mini) {
        if (anillo) ap_mem_soltar(anillo);
        if (jpg) ap_mem_soltar(jpg);
        if (arena_b) ap_mem_soltar(arena_b);
        if (ampl) ap_mem_soltar(ampl);
        if (mini) ap_mem_soltar(mini);
        anillo = jpg = arena_b = 0; ampl = mini = 0;
        return 0;
    }
    arena.b = arena_b; arena.cap = ARENA;
    return 1;
}

/* ================================================= LA LISTA: buscar y miniaturas ======================================================= */
static void pedir(int que)
{
    char q[150];
    if (!s.red_ok || s.pide) return;
    if (que == P_BUSCAR || que == P_MAS) {
        urlcod(q, sizeof q, consulta);
        copiar(s.url, ui_fmt(VID_SERV "/videos/buscar?q=%s&desde=%d&n=10", q, que == P_MAS ? n_res : 0), sizeof s.url);
        if (que == P_BUSCAR) avisar(ui_fmt("Buscando «%s»…", consulta));
    } else if (que == P_MINI) copiar(s.url, ui_fmt(VID_SERV "/videos/miniatura/%s?an=%d&al=%d", s.mini_pide, MINI_AN, MINI_AL), sizeof s.url);
    s.pide = que; s.canal = 0; s.n = 0; s.http = 0;
    if (red_estado() != W5_RED_LISTA && red_estado() != W5_RED_CONECTANDO) red_conectar(0);
}
static char *linea(char **p)
{
    char *a = *p, *q = a;
    while (*q && *q != '\n') q++;
    if (*q) { *q = 0; *p = q + 1; } else *p = q;
    return a;
}
static char *campo(char **p)
{
    char *a = *p, *q = a;
    while (*q && *q != '\t') q++;
    if (*q) { *q = 0; *p = q + 1; } else *p = q;
    return a;
}
static void leer_lista(int anadir)               /* «W5VID 1\t<n>\t<hay más>» y una línea «id\ttítulo\tcanal\tduración en s» por video */
{
    char *p = (char *)cuerpo, *l;
    int n0 = anadir ? n_res : 0;
    cuerpo[s.n < sizeof cuerpo ? s.n : sizeof cuerpo - 1] = 0;
    l = linea(&p);
    if (memcmp(l, "W5VID 1\t", 8)) { avisar("El computador contestó algo que no se entiende (¿SENUS al día?)."); return; }
    l += 8; campo(&l); hay_mas = *campo(&l) == '1';
    if (!anadir) n_res = 0;
    while (*p && n_res < MAXR) {
        char *id, *tit, *can, *dur;
        Res *r = &res[n_res];
        l = linea(&p);
        id = campo(&l); tit = campo(&l); can = campo(&l); dur = campo(&l);
        if (!*id) continue;
        copiar(r->id, id, sizeof r->id); copiar(r->titulo, tit, sizeof r->titulo); copiar(r->canal, can, sizeof r->canal);
        r->dur_s = 0; while (*dur >= '0' && *dur <= '9') r->dur_s = r->dur_s * 10u + (unsigned)(*dur++ - '0');
        n_res++;
    }
    if (!anadir) sel = 0;
    if (n_res == n0) avisar(anadir ? "No hay más." : ui_fmt("Nada para «%s».", consulta));
    else s.aviso[0] = 0;
}
static void acabada(void)
{
    int bien = red_codigo(s.canal) == W5_RCOD_BIEN && s.http == 200;
    if (s.pide == P_MINI) {
        if (bien && s.n == MINI_AN * MINI_AL * 2u) { memcpy(mini, cuerpo, MINI_AN * MINI_AL * 2u); s.mini_ok = 1; s.mini_version++; }
        copiar(s.mini_id, s.mini_pide, sizeof s.mini_id);   /* la que se pidió, con o sin éxito: no se pide sin parar */
    } else if (!bien) {
        char t[64]; red_texto(s.canal, t, sizeof t);
        avisar(s.http ? ui_fmt("El computador dice HTTP %d (¿SENUS está abierta?)", s.http) : ui_fmt("Sin respuesta: %s", t));
    } else if (s.pide == P_BUSCAR) { copiar(encabezado, ui_fmt("«%s»", consulta), sizeof encabezado); leer_lista(0); }
    else if (s.pide == P_MAS) leer_lista(1);
    red_cerrar(s.canal); s.canal = 0; s.pide = P_NADA;
}
static void cada(W5Ctx c)                        /* cada 50 ms: la petición de la lista en curso y la miniatura del elegido */
{
    static unsigned char b[2048];
    static int vuelta;
    int k;
    if (s.red_ok && s.pide && !s.canal) {
        int e = red_estado();
        if (e == W5_RED_LISTA) {
            s.canal = red_http("GET", s.url, 0, 0, 0, W5_RED_PLAZO(30));
            if (s.canal <= 0 && s.canal != W5_RED_E_OCUPADO && s.canal != W5_RED_E_SIN_CANAL) { avisar(ui_fmt("No se pudo pedir (%d).", s.canal)); s.pide = P_NADA; }
            if (s.canal < 0) s.canal = 0;
        } else if (e == W5_RED_FALLO) {
            avisar(red_fallo() == W5_REDF_ENLACE ? "Sin red: el computador no contesta. ¿SENUS está abierta y a menos de 10 m?"
                   : red_fallo() == W5_REDF_SIN_PUENTE ? "Sin red: elige el puente en Ajustes, Bluetooth." : ui_fmt("Sin red (fallo %d).", red_fallo()));
            s.pide = P_NADA;
        } else if (e == W5_RED_APAGADA) red_conectar(0);
    }
    if (s.canal > 0) {
        while ((k = red_recibir(s.canal, b, sizeof b)) > 0) {
            if (s.n < sizeof cuerpo) { unsigned m = (unsigned)k; if (m > sizeof cuerpo - s.n) m = sizeof cuerpo - s.n; memcpy(cuerpo + s.n, b, m); }
            s.n += (unsigned)k;
        }
        if (!s.http) s.http = red_http_estado(s.canal);
        if (red_canal(s.canal) == W5_RC_CERRADO && !red_disponibles(s.canal)) acabada();
    }
    /* la miniatura del elegido, cuando no hay otra cosa (y nunca mientras se ve un video: el enlace es para él) */
    if (s.red_ok && !s.pide && !v.activo && n_res && sel >= 0 && sel < n_res && mini && strcmp(res[sel].id, s.mini_id)) {
        if (strcmp(res[sel].id, s.mini_pide)) { copiar(s.mini_pide, res[sel].id, sizeof s.mini_pide); s.mini_ok = 0; s.mini_version++; }
        pedir(P_MINI);
    }
    if (!v.activo && (++vuelta % 5 == 0 || s.pide)) ui_sucio(c);
}

/* ================================================= VER: el reproductor ================================================================= */
static int reloj(void)                           /* ms del video que tocan ahora; -1 = aún no corre */
{
    W5MuRed r;
    if (v.propio) return (int)(v.propio_pausa ? v.propio_parado : v.propio_pts0 + (ap_ms() - v.propio_t0));
    if (!v.sonido) return -1;
    mu_red_info(&r);
    if (r.activa && r.tocando) { v.sonido_empezo = 1; return (int)r.pos_ms; }
    return -1;
}
static void reloj_propio(const char *porque)     /* el sonido no llega: el video sigue solo, desde el cuadro que se ve */
{
    if (v.propio) return;
    if (v.sonido) mu_parar();                    /* que no empiece luego, a destiempo */
    v.propio = 1; v.propio_pts0 = v.pts; v.propio_t0 = ap_ms(); v.propio_pausa = 0;
    copiar(v.sin_sonido, porque, sizeof v.sin_sonido);
    ap_log(ui_fmt("videos: sin sonido (%s): reloj propio desde %u ms\n", porque, v.pts));
}
static void pedir_video(u32 desde_ms)
{
    const Res *r = &res[v.i];
    if (desde_ms) copiar(v.url, ui_fmt(VID_SERV "/videos/%s.w5v?an=%d&al=%d&fps=%d&desde_ms=%u", r->id, AN, AL, FPS, desde_ms), sizeof v.url);
    else copiar(v.url, ui_fmt(VID_SERV "/videos/%s.w5v?an=%d&al=%d&fps=%d", r->id, AN, AL, FPS), sizeof v.url);
    v.desde_ms = desde_ms;
    w5v_anillo(&an, anillo, ANILLO);             /* lo que quedara del flujo anterior, fuera: el nuevo trae su cabecera */
    v.cab_ok = 0; v.http = 0; v.canal = 0;
    v.fase = red_estado() == W5_RED_LISTA ? R_PIDIENDO : R_CONECTANDO;
    if (v.fase == R_CONECTANDO && red_estado() != W5_RED_CONECTANDO) red_conectar(0);
}
static void parar_video(void)                    /* al salir del reproductor (por la tecla, o porque la pantalla se desecha) */
{
    if (!v.activo) return;
    if (v.canal > 0) red_cerrar(v.canal);
    v.canal = 0;
    if (w5_mured_hay()) mu_parar();
    ap_retener(W5_RET_ENERGIA, 0);
    mu_pantalla(0);
    if (v.dec_n) ap_log(ui_fmt("videos: fin de «%s»: %u cuadros, %u saltados, %u errores; decodificar %u ms, ampliar %u ms, pintar %u ms (medias x10: %u %u %u)\n",
                               res[v.i].id, v.dec_n, v.saltados, v.errores, DIV(v.dec_ms, v.dec_n), DIV(v.amp_ms, v.dec_n), DIV(v.pin_ms, v.pin_n),
                               DIV(v.dec_ms * 10u, v.dec_n), DIV(v.amp_ms * 10u, v.dec_n), DIV(v.pin_ms * 10u, v.pin_n)));
    v.activo = 0;
}
static int de_lado(void) { int t = pantalla_tam(); return (t >> 16) > (t & 0xFFFF); }
static void panel(int *w, int *h)                /* el tamaño del video en la orientación de ahora */
{
    if (de_lado()) { *w = LADO_AN; *h = LADO_AL; }
    else { *w = PAN_AN; *h = PAN_AL; }
}
static void ampliar(const ImgSalida *o, int W, int H)   /* la salida del decodificador a ampl, de W×H */
{
    if (o->w == W && o->h == H) memcpy(ampl, o->pix, (size_t)W * (size_t)H * 2u);
    else if (o->w * 2 == W && o->h * 2 == H) w5v_ampliar_2(o->pix, o->w, o->h, ampl);
    else if (o->w * 3 == W * 2 && o->h * 3 == H * 2 && !(o->w & 1) && !(o->h & 1)) w5v_ampliar_3_2(o->pix, o->w, o->h, ampl);
    else w5v_encajar(o->pix, o->w, o->h, ampl, W, H);
}
static void mostrar(u32 n, int nuevo)            /* decodifica el JPEG de jpg (n bytes), lo amplía a ampl y pide repintar. nuevo = 0: el mismo
                                                  * cuadro otra vez porque la pantalla giró (no cuenta en lo medido) */
{
    ImgSalida o;
    u32 t0 = ap_ms(), t1, t2;
    int W, H, r;
    panel(&W, &H);
    r = w5v_decodificar(&arena, jpg, n, (u16)W, (u16)H, &o);
    t1 = ap_ms();
    if (r < 0 || !o.pix) {
        v.errores++;
        if (!nuevo) { memset(ampl, 0, (size_t)W * (size_t)H * 2u); v.amp_w = W; v.amp_h = H; v.version++; }   /* negro, y no se reintenta sin parar */
        return;
    }
    ampliar(&o, W, H);
    t2 = ap_ms();
    v.amp_w = W; v.amp_h = H;
    v.img_w = o.w; v.img_h = o.h;
    if (nuevo) { v.dec_n++; v.dec_ms += t1 - t0; v.amp_ms += t2 - t1; v.seg_n++; }
    v.version++;
}
static void med_paso(W5Ctx c);
static void rep_cada(W5Ctx c)                    /* cada 10 ms: la red al anillo, el cuadro que toca, el sonido */
{
    u32 ahora = ap_ms(), n, pts;
    int rl, k;
    if (!v.activo) return;
    /* 1. la red */
    if (v.fase == R_CONECTANDO) {
        int e = red_estado();
        if (e == W5_RED_LISTA) v.fase = R_PIDIENDO;
        else if (e == W5_RED_FALLO) { copiar(v.aviso, red_fallo() == W5_REDF_ENLACE ? "Sin red: ¿SENUS está abierta?" : ui_fmt("Sin red (fallo %d).", red_fallo()), sizeof v.aviso); v.fase = R_FALLO; }
        else if (e == W5_RED_APAGADA) red_conectar(0);
    }
    if (v.fase == R_PIDIENDO) {
        v.canal = red_http("GET", v.url, 0, 0, 0, W5_RED_PLAZO(30));
        if (v.canal > 0) v.fase = R_LEYENDO;
        else if (v.canal != W5_RED_E_OCUPADO && v.canal != W5_RED_E_SIN_CANAL) { copiar(v.aviso, ui_fmt("No se pudo pedir el video (%d).", v.canal), sizeof v.aviso); v.fase = R_FALLO; }
        if (v.canal < 0) v.canal = 0;
    }
    if (v.fase == R_LEYENDO) {
        u32 tope = 24u * 1024u;                  /* lo más que se lee en una vuelta (la vuelta tiene que ser corta: el MP3 del sistema espera) */
        while (tope) {
            u8 *d;
            u32 h = w5v_hueco(&an, &d);
            if (!h) break;
            if (h > 4096) h = 4096;
            if (h > tope) h = tope;
            k = red_recibir(v.canal, d, h);
            if (k <= 0) break;
            w5v_escrito(&an, (u32)k);
            tope -= (u32)k;
        }
        if (!v.http) v.http = red_http_estado(v.canal);
        if (v.http && v.http != 200) {
            copiar(v.aviso, ui_fmt("El computador dice HTTP %d.", v.http), sizeof v.aviso);
            red_cerrar(v.canal); v.canal = 0; v.fase = R_FALLO;
        } else if (red_canal(v.canal) == W5_RC_CERRADO && !red_disponibles(v.canal)) {
            int bien = red_codigo(v.canal) == W5_RCOD_BIEN;
            char t[48];
            red_texto(v.canal, t, sizeof t);
            red_cerrar(v.canal); v.canal = 0;
            if (bien) v.fase = R_ACABADO;        /* lo que queda está en el anillo */
            else if (v.reintentos < 3 && !v.fin) {   /* se cortó: se vuelve a pedir desde donde va */
                v.reintentos++;
                rl = reloj();
                ap_log(ui_fmt("videos: el video se cortó (%s); se pide otra vez desde %d ms\n", t, rl > (int)v.pts ? rl : (int)v.pts + 1));
                pedir_video(rl > (int)v.pts ? (u32)rl : v.pts + 1u);
            } else { copiar(v.aviso, ui_fmt("El video se cortó: %s", t), sizeof v.aviso); v.fase = R_FALLO; }
        }
    }
    /* 2. el cuadro que toca */
    if (!v.cab_ok && an.n) {
        k = w5v_cabecera(&an, &v.cab);
        if (k == W5V_CUADRO) {
            v.cab_ok = 1;
            ap_log(ui_fmt("videos: %s %ux%u a %u fps, %u ms (desde %u)\n", res[v.i].id, v.cab.an, v.cab.al, v.cab.fps, v.cab.dur_ms, v.desde_ms));
        } else if (k == W5V_MAL) { copiar(v.aviso, "Lo que llegó no es un video (¿SENUS al día?).", sizeof v.aviso); if (v.canal > 0) red_cerrar(v.canal); v.canal = 0; v.fase = R_FALLO; }
    }
    if (v.cab_ok && !v.fin) {
        rl = reloj();
        if (rl >= 0 || !v.mostrado) {
            k = w5v_elegir(&an, rl, TARDE_MS, jpg, W5V_JPEG_MAX, &n, &pts, &v.saltados);
            if (k == W5V_CUADRO) {
                if (!v.mostrado) { v.t_primero = ahora; v.rec_ult = 0; v.t_rec_ult = ahora; }
                v.pts = pts; v.mostrado = 1; v.jpg_n = n;
                mostrar(n, 1);
                ui_sucio(c);
            } else if (k == W5V_FIN || (k == W5V_FALTA && v.fase == R_ACABADO && !w5v_cuadro(&an, 0, &n, &pts))) {
                v.fin = 1;
                if (v.propio && !v.propio_pausa) { v.propio_parado = v.pts; v.propio_pausa = 1; }   /* sin sonido: el reloj se queda en el último */
                ap_log(ui_fmt("videos: fin del video en %u ms\n", v.pts));
                ui_sucio(c);
            }
            else if (k == W5V_MAL) { copiar(v.aviso, "El video llegó dañado.", sizeof v.aviso); if (v.canal > 0) red_cerrar(v.canal); v.canal = 0; v.fase = R_FALLO; v.fin = 1; }
        }
    }
    /* 2b. la pantalla giró: el último cuadro (sigue en jpg), otra vez al tamaño nuevo; también en pausa y al acabar */
    if (v.mostrado && v.jpg_n && v.med_fase != 1) {
        int W, H;
        panel(&W, &H);
        if (W != v.amp_w || H != v.amp_h) { mostrar(v.jpg_n, 0); ui_sucio(c); }
    }
    /* 3. el sonido: si no empieza ni baja nada, el video sigue sin él */
    if (v.mostrado && !v.propio && !v.sonido_empezo) {
        if (!v.sonido) reloj_propio("este teléfono no lo toca");
        else {
            W5MuRed r;
            mu_red_info(&r);
            if (r.fase != W5_MURED_QUIETA) v.sonido_visto = 1;
            if (r.activa && r.tocando) v.sonido_empezo = 1;
            else if (r.fase == W5_MURED_FALLO) reloj_propio(r.porque[0] ? r.porque : "no se pudo bajar");
            else if (v.sonido_visto && r.fase == W5_MURED_QUIETA) reloj_propio(r.porque[0] ? r.porque : "no se pudo tocar");   /* la cola se paró sin sonar («red: no suena») */
            else {
                if (r.recibidos != v.rec_ult) { v.rec_ult = r.recibidos; v.t_rec_ult = ahora; }
                if (ahora - v.t_rec_ult > VID_ESPERA_SONIDO_MS) reloj_propio("no llega");
            }
        }
    }
    /* 4. lo medido: fps del último segundo, y al registro cada 5 s */
    if (ahora - v.seg_t >= 1000u) {
        v.fps10 = DIV(v.seg_n * 10000u, ahora - v.seg_t);
        v.seg_t = ahora; v.seg_n = 0;
        ui_sucio(c);                             /* la hora y la barra */
        k = ap_consulta(W5_Q_PILA_LIBRE);
        if (k >= 0 && (u32)k < v.pila_min) v.pila_min = (u32)k;
    }
    if (v.dec_n && !v.fin && ahora - v.t_log >= 5000u) {
        v.t_log = ahora;
        ap_log(ui_fmt("videos: reloj %d ms, %u cuadros, %u saltados, %u.%u fps; medias x10: decodificar %u, ampliar %u, pintar %u; anillo %u B, entrados %u B; pila libre %u\n",
                      reloj(), v.dec_n, v.saltados, v.fps10 / 10u, v.fps10 % 10u, DIV(v.dec_ms * 10u, v.dec_n), DIV(v.amp_ms * 10u, v.dec_n),
                      DIV(v.pin_ms * 10u, v.pin_n), an.n, an.entrados, v.pila_min));
    }
    if (v.med_fase) med_paso(c);
}
static void rep_pintar(W5Lienzo l, const W5Rect *r, void *dato)
{
    const int W = v.amp_w, H = v.amp_h;          /* lo que tiene ampl: justo al girar puede ser aún el de la otra orientación */
    int x, y;
    (void)dato;
    if (!v.mostrado || W <= 0 || H <= 0 || W > r->w || H > r->h) { li_rect(l, r->x, r->y, r->w, r->h, 0); return; }
    x = r->x + ((r->w - W) >> 1); y = r->y + ((r->h - H) >> 1);
    if (x > r->x) li_rect(l, r->x, r->y, x - r->x, r->h, 0);
    if (x + W < r->x + r->w) li_rect(l, x + W, r->y, r->x + r->w - x - W, r->h, 0);
    if (y > r->y) li_rect(l, x, r->y, W, y - r->y, 0);
    if (y + H < r->y + r->h) li_rect(l, x, y + H, W, r->y + r->h - y - H, 0);
    {
        u32 t0 = ap_ms();
        li_pixeles(l, x, y, W, H, ampl);
        v.pin_ms += ap_ms() - t0; v.pin_n++;
    }
}

/* # MEDIR: 5 decodificaciones y 10 ampliados del último cuadro seguidos, y 20 repintados encadenados (construir + pintar + volcar). Al registro. */
static void med_tras(W5Ctx c);
static void med_paso(W5Ctx c)
{
    if (v.med_fase == 1) {                       /* decodificar y ampliar: en esta vuelta */
        ImgSalida o;
        u32 t0, i;
        int ok = 1, W, H;
        panel(&W, &H);
        t0 = ap_ms();
        for (i = 0; i < 5 && ok; i++) ok = w5v_decodificar(&arena, jpg, v.jpg_n, (u16)W, (u16)H, &o) >= 0 && o.pix;   /* 5: la vuelta no pasa de ~0,15 s (el MP3) */
        v.med_dec = (ap_ms() - t0) * 2u;          /* x10 de lo de una */
        if (!ok) { ap_log("videos: medir: el último JPEG no se decodifica\n"); v.med_fase = 0; return; }
        t0 = ap_ms();
        for (i = 0; i < 10; i++) ampliar(&o, W, H);
        v.med_amp = ap_ms() - t0;
        v.amp_w = W; v.amp_h = H;
        v.med_fase = 2; v.med_cuenta = 0; v.med_t0 = ap_ms();
        v.version++; ui_sucio(c); ui_tras(c, 1, med_tras);
    }
}
static void med_tras(W5Ctx c)                    /* tras cada repintado: el siguiente, hasta 20 */
{
    if (v.med_fase != 2) return;
    if (++v.med_cuenta < 20) { v.version++; ui_sucio(c); ui_tras(c, 1, med_tras); return; }
    v.med_pin = ap_ms() - v.med_t0;
    v.med_fase = 0;
    ap_log(ui_fmt("videos: MEDIR (x10 = décimas de ms): decodificar %u, ampliar %u, repintar entero %u (20 seguidos: %u ms)\n",
                  v.med_dec, v.med_amp, v.med_pin / 2u, v.med_pin));
    copiar(v.aviso, ui_fmt("dec %u.%u · amp %u.%u · pint %u.%u ms", v.med_dec / 10u, v.med_dec % 10u, v.med_amp / 10u, v.med_amp % 10u,
                           v.med_pin / 20u, (v.med_pin % 20u) / 2u), sizeof v.aviso);
    ui_sucio(c);
}

static int pausado(void)
{
    W5MuInfo m;
    if (v.propio) return v.propio_pausa;
    if (!v.sonido_empezo) return 0;
    mu_info(&m);
    return m.estado == W5_MU_PAUSA;
}
static int rep_tecla(W5Ctx t, int k, void *dato)
{
    (void)dato;
    if (k == W5K_OK || k == W5K_SEL_IZQ) {
        if (v.fin) return 1;                     /* acabado: no hay nada que pausar */
        if (v.propio) {
            if (v.propio_pausa) { v.propio_pausa = 0; v.propio_pts0 = v.propio_parado; v.propio_t0 = ap_ms(); }
            else { v.propio_parado = (u32)reloj(); v.propio_pausa = 1; }
        } else if (v.sonido_empezo) mu_alternar();
        ui_sucio(ui_mi_pantalla(t));
        return 1;
    }
    if (k == W5K_ALM) {                          /* # medir */
        if (v.mostrado && !v.med_fase) { v.med_fase = 1; copiar(v.aviso, "Midiendo…", sizeof v.aviso); ui_sucio(ui_mi_pantalla(t)); }
        return 1;
    }
    if (k == W5K_ATRAS || k == W5K_SEL_DER) { parar_video(); ui_sacar(); return 1; }
    return ui_teclas_defecto(t, k);
}
static void rep_iniciar(W5Ctx c, void *dato)
{
    (void)dato;
    ap_retener(W5_RET_ENERGIA, 1);               /* que la pantalla no se apague mientras se ve */
    mu_pantalla(1);                              /* Vol± son del sonido */
    v.seg_t = v.t_log = ap_ms();
    v.pila_min = 0xFFFFFFFFu;
    ui_cada(c, 10, rep_cada);
}
static void rep_desechar(W5Ctx c, void *dato) { (void)c; (void)dato; parar_video(); if (yo) ui_sucio(yo); }
static W5W rep_construir(W5Ctx c, void *dato)
{
    unsigned tenue = ui_color(W5_COL_TENUE);
    const Res *r = &res[v.i];
    int rl = v.mostrado ? reloj() : -1, pa = pausado();
    u32 pos = rl >= 0 ? (u32)rl : v.pts, dur = v.cab.dur_ms ? v.cab.dur_ms : r->dur_s * 1000u;
    const char *estado = v.fase == R_FALLO ? "No se pudo" : v.fase == R_CONECTANDO ? "Conectando…" : !v.cab_ok ? "Pidiendo…"
                       : !v.mostrado ? "Preparando…" : v.fin ? "Fin" : pa ? "Pausa" : rl < 0 ? "Cargando sonido…"
                       : v.propio ? "Sin sonido" : ui_fmt("%u.%u fps", v.fps10 / 10u, v.fps10 % 10u);
    const char *aviso = v.aviso[0] ? v.aviso : v.propio && !v.fin ? ui_fmt("Sin sonido: %s.", v.sin_sonido) : "";
    (void)c; (void)dato;
    if (dur && pos > dur) pos = dur;
    if (de_lado()) {                             /* a pantalla completa: arriba el título (o el aviso), en medio el video × 2, abajo el tiempo */
        const unsigned blanco = W5_RGB(255, 255, 255), gris = W5_RGB(170, 170, 170);
        return Teclas(rep_tecla, Caja(A_AN(W5_INF), A_AL(W5_INF), A_COLOR(W5_RGB(0, 0, 0)), A_HIJO(Columna(A_CRUZADO(W5P_ESTIRAR), HIJOS(
            Expandido(RellenoXY(8, 0, Columna(A_PRINCIPAL(W5P_CENTRO), A_CRUZADO(W5P_ESTIRAR), HIJOS(
                Parrafo(aviso[0] ? aviso : r->titulo, A_COLOR(aviso[0] ? W5_RGB(255, 176, 64) : blanco), A_MAX_LINEAS(1),
                        A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA))))))),
            Pintura(rep_pintar, A_VERSION(v.version), A_AN(W5_INF), A_AL(LADO_AL)),
            Expandido(RellenoXY(8, 0, Columna(A_PRINCIPAL(W5P_CENTRO), A_CRUZADO(W5P_ESTIRAR), HIJOS(
                Fila(A_ESPACIO(8), HIJOS(
                    Texto(ui_fmt("%s / %s", mmss(pos), dur ? mmss(dur) : "-:--"), A_COLOR(gris), A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA))),
                    Expandido(Progreso((int)(pos / 1000u), (int)(dur >= 1000u ? dur / 1000u : 1u), A_FONDO(W5_RGB(64, 64, 64)))),
                    Texto(estado, A_COLOR(gris), A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA))))))))))))));
    }
    return Teclas(rep_tecla, ui_andamio("Videos", Columna(A_CRUZADO(W5P_ESTIRAR), HIJOS(
        Pintura(rep_pintar, A_VERSION(v.version), A_AN(W5_INF), A_AL(PAN_AL)),
        RellenoXY(6, 4, Columna(A_CRUZADO(W5P_ESTIRAR), A_ESPACIO(3), HIJOS(
            Parrafo(r->titulo, A_MAX_LINEAS(2)),
            Texto(r->canal, A_COLOR(tenue), A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA))),
            Progreso((int)(pos / 1000u), (int)(dur >= 1000u ? dur / 1000u : 1u), A_FONDO(ui_color(W5_COL_BARRA))),
            Fila(HIJOS(
                Texto(ui_fmt("%s / %s", mmss(pos), dur ? mmss(dur) : "-:--"), A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA))),
                Hueco(),
                Texto(estado, A_COLOR(tenue), A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA))))),
            Parrafo(aviso, A_COLOR(tenue), A_MAX_LINEAS(2), A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA)))))))),   /* errores y lo medido (#) */
        v.fin ? "" : pa ? "Seguir" : "Pausa", "Atrás"));
}
static void ver(int i)
{
    const Res *r;
    if (i < 0 || i >= n_res) return;
    if (!s.red_ok) { ui_aviso("Videos", "Aquí no hay red: hace falta el W580 con el puente del computador (SENUS, Conexión)."); return; }
    if (!memoria()) { ui_aviso("Videos", "No hay memoria para el video."); return; }
    if (s.canal > 0) { red_cerrar(s.canal); s.canal = 0; }   /* lo que pidiera la lista (una miniatura), fuera: el enlace es para el video */
    s.pide = P_NADA;
    r = &res[i];
    memset(&v, 0, sizeof v);
    v.i = i; v.activo = 1;
    v.sonido = w5_mured_hay();
    if (v.sonido) {                              /* el sonido: el MP3 de Música, por la cola del sistema */
        int k;
        mu_red_vaciar();
        k = mu_red_poner(ui_fmt(VID_SERV "/pista/%s.mp3", r->id), r->titulo, r->canal, r->dur_s * 1000u);
        if (k < 0 || mu_red_tocar(k) != W5_OK) v.sonido = 0;
    }
    pedir_video(0);
    ap_log(ui_fmt("videos: ver %s «%s» (%u s); sonido %s\n", r->id, r->titulo, r->dur_s, v.sonido ? "por la cola de la red" : "NO"));
    ui_empujar(ui_pantalla(rep_construir, rep_iniciar, rep_desechar, 0));
}

/* ================================================= BUSCAR ============================================================================== */
static W5Et ent;
static int bu_tecla(W5Ctx t, int k, void *dato)
{
    (void)dato;
    if (k == W5K_OK || k == W5K_SEL_IZQ) {
        const char *q = ent ? et_texto(ent) : "";
        if (q && q[0]) { copiar(consulta, q, sizeof consulta); pedir(P_BUSCAR); }
        ui_sacar();
        return 1;
    }
    return ui_teclas_defecto(t, k);
}
static void bu_cambio(W5Ctx c, void *d) { (void)d; ui_sucio(ui_mi_pantalla(c)); }
static W5W bu_construir(W5Ctx c, void *dato)
{
    (void)c; (void)dato;
    return Teclas(bu_tecla, ui_andamio("Buscar", Columna(A_CRUZADO(W5P_ESTIRAR), A_ESPACIO(6), HIJOS(
        RellenoXY(8, 6, Texto("Video, canal o tema:")),
        RellenoXY(6, 0, CampoTexto(ent, A_LINEAS(1), A_AUTOFOCO(1), A_AL_CAMBIAR(bu_cambio))),
        RellenoXY(8, 4, Parrafo("OK busca: salen 10 y, al llegar al final de la lista, 10 más. La * cambia entre abc, Abc, ABC y 123; mantenla para DICTAR.")),
        Expandido(Hueco()),
        RellenoXY(8, 4, Texto(ui_fmt("%u letras · %s", ent ? et_len(ent) : 0u, ent ? et_modo(ent) : "abc"), A_COLOR(ui_color(W5_COL_TENUE)),
                              A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA)))))),
        "Buscar", "Cancelar"));
}
static void buscar(void)
{
    if (!s.red_ok) { ui_aviso("Videos", "Aquí no hay red: hace falta el W580 con el puente del computador (SENUS, Conexión)."); return; }
    if (!ent) ent = et_nueva(sizeof consulta);
    if (!ent) { ui_aviso("Buscar", "No hay memoria para el campo de texto."); return; }
    et_poner(ent, consulta);
    ui_empujar(ui_pantalla(bu_construir, 0, 0, 0));
}

/* ================================================= LA PANTALLA DE LA LISTA ============================================================ */
static void activar(W5Ctx c, int i, void *dato) { (void)c; (void)dato; ver(i); }
static void mover(W5Ctx c, int i, void *dato)
{
    (void)dato;
    if (i >= n_res - 1 && hay_mas && !s.pide) pedir(P_MAS);   /* al llegar abajo: 10 más */
    ui_sucio(ui_mi_pantalla(c));                 /* la cabecera: el elegido y su miniatura */
}
static int tecla(W5Ctx t, int k, void *dato)
{
    (void)dato;
    if (k == W5K_SEL_IZQ || (k == W5K_OK && !n_res)) { buscar(); return 1; }
    return ui_teclas_defecto(t, k);
}
static void pintar_mini(W5Lienzo l, const W5Rect *r, void *dato)
{
    (void)dato;
    if (s.mini_ok && mini) li_pixeles(l, r->x, r->y, MINI_AN, MINI_AL, mini);
    else {
        li_rect(l, r->x, r->y, MINI_AN, MINI_AL, ui_color(W5_COL_BARRA));
        li_rect(l, r->x + MINI_AN / 2 - 6, r->y + MINI_AL / 2 - 6, 12, 12, ui_color(W5_COL_TENUE));
    }
}
static W5W fila(W5Ctx c, int i, int sl, void *dato)
{
    (void)c; (void)dato;
    if (i < 0 || i >= n_res) return ui_fila("", 0, sl);
    return ui_fila(ui_fmt("%s · %s", res[i].titulo, res[i].canal), mmss(res[i].dur_s * 1000u), sl);
}
static void iniciar(W5Ctx c, void *dato)
{
    (void)dato;
    yo = c;
    mu_abrir();
    memoria();                                   /* ya: la miniatura la usa (si no hay, ver() lo vuelve a intentar) */
    s.red_ok = w5_mured_hay() && w5_red_hay();
    if (s.red_ok) {
        if (red_estado() != W5_RED_LISTA) red_conectar(0);
        if (!n_res) avisar("Buscar: escribe qué quieres ver.");
    } else avisar("Aquí no hay red: esta app necesita el W580 con el puente del computador (SENUS, Conexión).");
    ui_cada(c, 50, cada);
}
static void desechar(W5Ctx c, void *dato)
{
    (void)c; (void)dato;
    yo = 0;
    parar_video();
    if (s.canal > 0) red_cerrar(s.canal);
    s.canal = 0; s.pide = P_NADA;
}
static W5W construir(W5Ctx c, void *dato)
{
    unsigned tenue = ui_color(W5_COL_TENUE);
    const Res *r = n_res && sel >= 0 && sel < n_res ? &res[sel] : 0;
    (void)c; (void)dato;
    return Teclas(tecla, ui_andamio("Videos", Columna(A_CRUZADO(W5P_ESTIRAR), HIJOS(
        RellenoXY(6, 4, Fila(A_ESPACIO(8), A_CRUZADO(W5P_CENTRO), HIJOS(
            Pintura(pintar_mini, A_VERSION(s.mini_version), A_AN(MINI_AN), A_AL(MINI_AL)),
            Expandido(Columna(A_CRUZADO(W5P_ESTIRAR), A_ESPACIO(2), HIJOS(
                Parrafo(r ? r->titulo : n_res ? "" : "Busca un video", A_MAX_LINEAS(2)),
                Texto(r ? ui_fmt("%s · %s", r->canal, mmss(r->dur_s * 1000u)) : encabezado, A_COLOR(tenue), A_MARCAS(W5_F_ESTILO(W5_T_PEQUENA))))))))),
        Caja(A_AN(W5_INF), A_AL(1), A_COLOR(tenue)),
        Expandido(n_res && s.pide != P_BUSCAR
                  ? ListaPerezosa(n_res, fila, A_AL_ACTIVAR(activar), A_AL_MOVER(mover), A_SEL(&sel), A_ALTO_FILA(0))
                  : RellenoXY(8, 8, Parrafo(s.aviso, A_COLOR(tenue)))))),
        "Buscar", "Atrás"));
}

W5Pal app_suceso(int suceso, W5Pal a, W5Pal b)
{
    (void)a; (void)b;
    if (suceso == W5_ABRIR) return (W5Pal)ui_pantalla(construir, iniciar, desechar, 0);
    if (suceso == W5_SALIR) { parar_video(); if (s.canal > 0) red_cerrar(s.canal); s.canal = 0; }
    return 0;
}
