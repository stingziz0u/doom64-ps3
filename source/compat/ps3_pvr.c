/* ps3_pvr.c -- emulador del TA (Tile Accelerator) del PowerVR2 sobre el RSX.
 *
 * ---------------------------------------------------------------------------
 * COMO DIBUJA EL MOTOR
 *
 * doom64-dc no usa OpenGL: le habla al PVR del Dreamcast en su idioma nativo,
 * un STREAM de registros de 32 bytes que se manda al TA:
 *
 *   [header de poligono]  estado: textura, blending, profundidad, fog...
 *   [vertice]             x, y en pixeles de 640x480, z = 1/w, u, v, argb, oargb
 *   [vertice]
 *   [vertice EOL]         fin de la tira de triangulos
 *   [vertice] ...         otra tira con el mismo estado
 *   [header] ...          cambio de estado
 *
 * Los vertices llegan YA PROYECTADOS: el motor hace la transformacion, el
 * clipping contra el near plane y la division perspectiva el mismo, en CPU.
 *
 * Hay tres listas que el PVR dibuja en orden: OPACA, PUNCH-THROUGH (opaca
 * con agujeros) y TRANSLUCIDA. El motor escribe en ellas por tres caminos:
 *   - sq_fast_cpy(SQ_MASK_DEST(PVR_TA_INPUT), ...)   directo al TA, a la
 *     lista abierta con pvr_list_begin
 *   - pvr_list_prim(lista, datos, bytes)             a una lista dada
 *   - pvr_vertbuf_tail() + pvr_vertbuf_written()     el motor escribe EN
 *     NUESTRA memoria y despues avisa cuanto escribio (el camino de las
 *     paredes y pisos, que hace el clipping en el lugar)
 *
 * ---------------------------------------------------------------------------
 * COMO LO EMULAMOS
 *
 * Los tres caminos terminan en el mismo buffer por lista. En
 * pvr_scene_finish se recorre cada lista (OP, PT, TR), se interpretan los
 * headers, se convierten las tiras a triangulos sueltos y se le entregan al
 * backend RSX (ps3_rsx.c) en lotes con el mismo estado.
 *
 * El motor no se toca: todo lo que el PVR hacia por hardware se hace aca.
 *
 * ETAPA 3a: geometria sin texturas. Cada poligono se pinta con el color de
 * iluminacion que calcula el motor (argb + oargb, exactamente como el PVR
 * con "specular" activado), con fog, y los texturizados con un tono de gris
 * derivado de la textura para que las superficies se distingan. Los sprites
 * 2D (HUD, fuentes) todavia no se dibujan.
 * ------------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <math.h>

#include "kos.h"
#include "dc/pvr.h"
#include "../ps3_log.h"
#include "../ps3_rsx.h"

/* ================================================================== */
/* Buffers de las listas                                               */
/* ================================================================== */

#define NUM_LISTS       5
#define LIST_BUF_SIZE   (4 * 1024 * 1024)    /* ~130.000 registros por lista */
#define TAIL_RESERVE    (8 * 32)             /* init_poly usa hasta header + 5 vertices */

typedef struct {
    uint8_t *buf;
    uint32_t len;
    int      tail_scratch;    /* la ultima tail() devolvio el buffer de descarte */
    int      overflowed;
} ta_list_t;

static ta_list_t s_list[NUM_LISTS];
static int       s_cur_list = -1;

/* Donde escribe el motor cuando la lista se lleno: se descarta. */
static uint8_t s_scratch[TAIL_RESERVE * 2] __attribute__((aligned(32)));

static pvr_vertex_t s_dr_vert[2] __attribute__((aligned(32)));
static int          s_dr_idx = 0;

static int  s_frame = 0;
static int  s_warned_nolist = 0;

static int list_ok(int l) { return l >= 0 && l < NUM_LISTS; }

static int list_alloc(int l)
{
    if (s_list[l].buf) return 1;
    s_list[l].buf = (uint8_t *)memalign(32, LIST_BUF_SIZE);
    if (!s_list[l].buf) {
        ps3_logf("[pvr] *** sin memoria para la lista %d ***", l);
        return 0;
    }
    return 1;
}

static void ta_append(int l, const void *data, uint32_t bytes)
{
    ta_list_t *L;

    if (!list_ok(l)) {
        if (!s_warned_nolist) {
            ps3_logf("[pvr] envio al TA sin lista abierta (lista %d): va a la opaca", l);
            s_warned_nolist = 1;
        }
        l = PVR_LIST_OP_POLY;
    }
    L = &s_list[l];
    if (!list_alloc(l)) return;

    if (L->len + bytes > LIST_BUF_SIZE) {
        L->overflowed = 1;
        return;
    }
    memcpy(L->buf + L->len, data, bytes);
    L->len += bytes;
}

/* ================================================================== */
/* API del TA                                                           */
/* ================================================================== */

int pvr_init(pvr_init_params_t *p)  { (void)p; return 0; }
int pvr_shutdown(void)              { return 0; }

static uint32_t s_bg_rgb = 0x000000;

void pvr_set_bg_color(float r, float g, float b)
{
    int ir = (int)(r * 255.0f), ig = (int)(g * 255.0f), ib = (int)(b * 255.0f);
    ir = ir < 0 ? 0 : (ir > 255 ? 255 : ir);
    ig = ig < 0 ? 0 : (ig > 255 ? 255 : ig);
    ib = ib < 0 ? 0 : (ib > 255 ? 255 : ib);
    s_bg_rgb = ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | (uint32_t)ib;
}

void pvr_set_zclip(float zc) { (void)zc; }

int pvr_scene_begin(void)
{
    int i;
    for (i = 0; i < NUM_LISTS; i++) {
        s_list[i].len = 0;
        s_list[i].tail_scratch = 0;
        s_list[i].overflowed = 0;
    }
    s_cur_list = -1;
    return 0;
}

int pvr_list_begin(int list)  { s_cur_list = list; return 0; }
int pvr_list_finish(void)     { s_cur_list = -1;   return 0; }

int pvr_list_prim(int list, void *data, int size)
{
    if (size > 0) ta_append(list, data, (uint32_t)size);
    return 0;
}

/* Store queues del SH4. OJO: en KOS el tercer argumento es la cantidad de
 * BLOQUES DE 32 BYTES, no de bytes. Y SQ_MASK_DEST(PVR_TA_INPUT) es NULL:
 * eso significa "al TA", a la lista abierta. */
void sq_fast_cpy(void *dest, const void *src, size_t n)
{
    if (!src || !n) return;
    if (dest == SQ_MASK_DEST(PVR_TA_INPUT))
        ta_append(s_cur_list, src, (uint32_t)(n * 32));
    else
        memcpy(dest, src, n * 32);
}

void sq_cpy(void *dest, const void *src, size_t n)          /* n en bytes */
{
    if (!src || !n) return;
    if (dest == SQ_MASK_DEST(PVR_TA_INPUT))
        ta_append(s_cur_list, src, (uint32_t)n);
    else
        memcpy(dest, src, n);
}

void sq_set32(void *dest, uint32_t c, size_t n)
{
    uint32_t *d = (uint32_t *)dest;
    size_t i;
    if (!d) return;
    for (i = 0; i < n / 4; i++) d[i] = c;
}

/* Direct render: el motor pide un vertice, lo llena y lo "commitea". */
void pvr_dr_init(pvr_dr_state_t *s) { if (s) *s = 0; }

pvr_vertex_t *ps3_pvr_dr_target(void)
{
    s_dr_idx ^= 1;
    return &s_dr_vert[s_dr_idx];
}

void pvr_dr_commit(void *addr)
{
    ta_append(s_cur_list, addr, 32);
}

/* Vertex buffers: el motor escribe directo en nuestra memoria. */
void pvr_set_vertbuf(int l, void *b, int n) { (void)l; (void)b; (void)n; }

void *pvr_vertbuf_tail(int l)
{
    ta_list_t *L;
    if (!list_ok(l) || !list_alloc(l)) return s_scratch;
    L = &s_list[l];
    if (L->len + TAIL_RESERVE > LIST_BUF_SIZE) {
        L->tail_scratch = 1;
        L->overflowed = 1;
        return s_scratch;
    }
    L->tail_scratch = 0;
    return L->buf + L->len;
}

void pvr_vertbuf_written(int l, uint32_t amt)
{
    ta_list_t *L;
    if (!list_ok(l)) return;
    L = &s_list[l];
    if (L->tail_scratch) { L->tail_scratch = 0; return; }
    if (L->len + amt <= LIST_BUF_SIZE)
        L->len += amt;
}

/* ================================================================== */
/* Contextos y headers                                                  */
/*                                                                      */
/* El header se compila con el LAYOUT REAL del PVR, bit por bit igual   */
/* que pvr_poly_compile de KallistiOS. No es un detalle: el motor       */
/* escribe directo sobre los bits del header ya compilado               */
/* (r_phase3.c:1168-1187 y 1678):                                       */
/*                                                                      */
/*   cmd   bits 24-26   cambia la lista (TR <-> PT) segun la calidad     */
/*   mode2 bits 26-31   fuerza el blending (0x94000000 = SRCALPHA/INV)   */
/*   mode2 bits 12-19   filtro y espejado de la textura                  */
/*                                                                      */
/* Con un encoding propio, esas escrituras pisaban el formato de la     */
/* textura y las paredes salian como ruido. Con el layout real, el      */
/* motor toca lo que cree que toca y aca se lee igual que el hardware.  */
/*                                                                      */
/*   cmd   PVR_CMD_POLYHDR | txr<<3 | specular<<2 | shading<<1 | list<<24 */
/*   mode1 depthcmp<<29 | culling<<27 | depthwrite<<26 | txr<<25         */
/*   mode2 src<<29 | dst<<26 | srcen<<25 | dsten<<24 | fog<<22 |         */
/*         clamp<<21 | alpha<<20 | txralpha_off<<19 | flip<<17 |         */
/*         uvclamp<<15 | filter<<12 | env<<6 | usize<<3 | vsize          */
/*   mode3 formato (bits 21-31) | direccion en VRAM >> 3                 */
/*                                                                      */
/* La direccion de 21 bits no alcanza para un puntero de la PS3, asi    */
/* que el puntero completo va en d1/d2 (en KOS quedan en 0xffffffff y   */
/* el TA los ignora) y d4 lleva una firma.                              */
/* ================================================================== */

#define HDR_SIGNATURE   0x44363448u      /* 'D64H' */
#define CMD_SPRITEHDR   0xA0000000u

static void cxt_defaults(pvr_poly_cxt_t *dst, int list)
{
    /* Los mismos defaults que KOS (pvr_poly_cxt_col/txr). Importan: el motor
     * solo pisa algunos campos antes de compilar, y el resto sale de aca.
     * En especial fog_type: PVR_FOG_TABLE vale 0, asi que un contexto en
     * cero tendria fog sin que nadie lo haya pedido. */
    int alpha = list > PVR_LIST_OP_MOD;

    memset(dst, 0, sizeof(*dst));
    dst->list_type        = list;
    dst->gen.shading      = PVR_SHADE_GOURAUD;
    dst->gen.culling      = PVR_CULLING_CCW;
    dst->gen.fog_type     = PVR_FOG_DISABLE;
    dst->gen.fog_type2    = PVR_FOG_DISABLE;
    dst->gen.color_clamp  = PVR_CLRCLAMP_DISABLE;
    dst->depth.comparison = PVR_DEPTHCMP_GREATER;
    dst->depth.write      = PVR_DEPTHWRITE_ENABLE;

    if (!alpha) {
        dst->gen.alpha = PVR_ALPHA_DISABLE;
        dst->blend.src = PVR_BLEND_ONE;
        dst->blend.dst = PVR_BLEND_ZERO;
    } else {
        dst->gen.alpha = PVR_ALPHA_ENABLE;
        dst->blend.src = PVR_BLEND_SRCALPHA;
        dst->blend.dst = PVR_BLEND_INVSRCALPHA;
    }
    dst->blend.src_enable = 0;
    dst->blend.dst_enable = 0;
}

void pvr_poly_cxt_col(pvr_poly_cxt_t *dst, int list)
{
    if (!dst) return;
    cxt_defaults(dst, list);
    dst->txr.enable = 0;
}

void pvr_poly_cxt_txr(pvr_poly_cxt_t *dst, int list, int fmt,
                      int tw, int th, pvr_ptr_t addr, int filter)
{
    if (!dst) return;
    cxt_defaults(dst, list);
    dst->txr.enable   = 1;
    dst->txr.filter   = filter;
    dst->txr.mipmap   = PVR_MIPMAP_DISABLE;
    dst->txr.uv_flip  = PVR_UVFLIP_NONE;
    dst->txr.uv_clamp = PVR_CLAMP_NONE;
    dst->txr.alpha    = (list > PVR_LIST_OP_MOD) ? PVR_ALPHA_ENABLE : PVR_ALPHA_DISABLE;
    dst->txr.env      = PVR_TXRENV_MODULATEALPHA;
    dst->txr.width    = tw;
    dst->txr.height   = th;
    dst->txr.format   = fmt;
    dst->txr.base     = addr;
}

void pvr_sprite_cxt_col(pvr_sprite_cxt_t *dst, int list)
{ pvr_poly_cxt_col(dst, list); }

void pvr_sprite_cxt_txr(pvr_sprite_cxt_t *dst, int list, int fmt,
                        int tw, int th, pvr_ptr_t addr, int filter)
{ pvr_poly_cxt_txr(dst, list, fmt, tw, th, addr, filter); }

/* 8,16,...,1024 -> 0..7 (el campo de tamano del PVR) */
static uint32_t size_code(int n)
{
    uint32_t c = 0;
    while (c < 7 && (8 << c) < n) c++;
    return c;
}

static void compile_words(const pvr_poly_cxt_t *c, uint32_t base_cmd,
                          uint32_t *cmd, uint32_t *m1, uint32_t *m2, uint32_t *m3)
{
    int txr = c->txr.enable ? 1 : 0;

    *cmd  = base_cmd;
    if (txr) *cmd |= 8;
    *cmd |= ((uint32_t)c->list_type & 7) << 24;
    *cmd |= ((uint32_t)c->gen.specular & 1) << 2;
    *cmd |= ((uint32_t)c->gen.shading  & 1) << 1;

    *m1  = ((uint32_t)c->depth.comparison & 7) << 29;
    *m1 |= ((uint32_t)c->gen.culling      & 3) << 27;
    *m1 |= ((uint32_t)c->depth.write      & 1) << 26;
    *m1 |= (uint32_t)txr << 25;

    *m2  = ((uint32_t)c->blend.src        & 7) << 29;
    *m2 |= ((uint32_t)c->blend.dst        & 7) << 26;
    *m2 |= ((uint32_t)(c->blend.src_enable ? 1 : 0)) << 25;
    *m2 |= ((uint32_t)(c->blend.dst_enable ? 1 : 0)) << 24;
    *m2 |= ((uint32_t)c->gen.fog_type     & 3) << 22;
    *m2 |= ((uint32_t)c->gen.color_clamp  & 1) << 21;
    *m2 |= ((uint32_t)(c->gen.alpha == PVR_ALPHA_ENABLE ? 1 : 0)) << 20;

    if (!txr) {
        *m3 = 0;
        return;
    }
    /* En el hardware el bit 19 es "ignorar el alfa de la textura". */
    *m2 |= ((uint32_t)(c->txr.alpha == PVR_ALPHA_ENABLE ? 0 : 1)) << 19;
    *m2 |= ((uint32_t)c->txr.uv_flip  & 3) << 17;
    *m2 |= ((uint32_t)c->txr.uv_clamp & 3) << 15;
    *m2 |= ((uint32_t)c->txr.filter   & 7) << 12;
    *m2 |= ((uint32_t)c->txr.env      & 3) << 6;
    *m2 |= size_code(c->txr.width)  << 3;
    *m2 |= size_code(c->txr.height);

    *m3  = ((uint32_t)(c->txr.mipmap ? 1 : 0)) << 31;
    *m3 |= (uint32_t)c->txr.format & 0xffe00000u;
    *m3 |= (uint32_t)(((uintptr_t)c->txr.base & 0x00fffff8u) >> 3);
}

static void encode_ptr(uint32_t *hi, uint32_t *lo, pvr_ptr_t p)
{
    uint64_t v = (uint64_t)(uintptr_t)p;
    *hi = (uint32_t)(v >> 32);
    *lo = (uint32_t)v;
}

void pvr_poly_compile(pvr_poly_hdr_t *dst, pvr_poly_cxt_t *src)
{
    if (!dst || !src) return;
    compile_words(src, PVR_CMD_POLYHDR, &dst->cmd, &dst->mode1, &dst->mode2, &dst->mode3);
    encode_ptr(&dst->d1, &dst->d2, src->txr.base);
    dst->d3 = 0;
    dst->d4 = HDR_SIGNATURE;
}

/* El header de sprite trae ademas argb/oargb (el color de todo el sprite:
 * el motor los pisa para tenir el HUD); el puntero va en d1/d2. */
void pvr_sprite_compile(pvr_sprite_hdr_t *dst, pvr_sprite_cxt_t *src)
{
    if (!dst || !src) return;
    compile_words(src, CMD_SPRITEHDR, &dst->cmd, &dst->mode1, &dst->mode2, &dst->mode3);
    dst->argb  = 0xffffffffu;
    dst->oargb = 0;
    encode_ptr(&dst->d1, &dst->d2, src->txr.base);
}

/* ================================================================== */
/* Memoria de texturas del PVR                                          */
/*                                                                      */
/* En el Dreamcast son 8 MB de VRAM. Aca es memoria normal, pero se     */
/* lleva un registro de cada bloque porque hace falta saber dos cosas   */
/* al convertir una textura:                                            */
/*                                                                      */
/*  - LINEAL O TWIDDLED. El PVR guarda las texturas en orden "twiddled"  */
/*    (Morton). pvr_txr_load copia tal cual (el dato ya viene twiddled   */
/*    del wadtool), pero pvr_txr_load_ex en KOS twiddlea al cargar: el   */
/*    motor le pasa datos lineales. Aca se copian lineales y se anota.   */
/*  - GENERACION. El motor reusa bloques (el cache de sprites de         */
/*    enemigos): cada carga sube la generacion e invalida la textura     */
/*    RSX que se habia armado con el contenido viejo.                    */
/*                                                                      */
/* Ademas pvr_mem_available devuelve lo que queda del presupuesto, asi la*/
/* logica del motor para vaciar el cache de sprites funciona igual que   */
/* en el Dreamcast.                                                      */
/* ================================================================== */

#define PVR_VRAM_BYTES  (32 * 1024 * 1024)   /* PS3: el PVR real tiene 8 MB */
#define MEMREC_MAX      8192                  /* potencia de 2 */

typedef struct {
    uintptr_t ptr;
    uint32_t  size;
    uint32_t  gen;
    uint8_t   linear;
    uint8_t   state;        /* 0 libre, 1 usado, 2 borrado */
    uint8_t   pending;      /* liberado por el motor, se libera al final del frame */
} memrec_t;

static memrec_t s_mem[MEMREC_MAX];
static uint32_t s_mem_bytes = 0;

static uint32_t ptr_hash(uintptr_t p)
{
    uint64_t v = (uint64_t)p >> 5;
    return (uint32_t)((v * 2654435761u) ^ (v >> 17));
}

static memrec_t *mem_find(uintptr_t p)
{
    uint32_t i, h = ptr_hash(p);
    for (i = 0; i < MEMREC_MAX; i++) {
        memrec_t *r = &s_mem[(h + i) & (MEMREC_MAX - 1)];
        if (r->state == 0) return NULL;
        if (r->state == 1 && r->ptr == p) return r;
    }
    return NULL;
}

static memrec_t *mem_insert(uintptr_t p)
{
    uint32_t i, h = ptr_hash(p);
    for (i = 0; i < MEMREC_MAX; i++) {
        memrec_t *r = &s_mem[(h + i) & (MEMREC_MAX - 1)];
        if (r->state != 1) {
            memset(r, 0, sizeof(*r));
            r->ptr = p;
            r->state = 1;
            return r;
        }
    }
    return NULL;
}

static void tex_invalidate(uintptr_t p);   /* mas abajo */

pvr_ptr_t pvr_mem_malloc(size_t size)
{
    void *p = memalign(32, size ? size : 32);
    memrec_t *r;
    if (!p) return NULL;
    r = mem_insert((uintptr_t)p);
    if (r) {
        r->size = (uint32_t)size;
        s_mem_bytes += (uint32_t)size;
    }
    return p;
}

static void mem_free_now(pvr_ptr_t p)
{
    memrec_t *r;
    tex_invalidate((uintptr_t)p);
    r = mem_find((uintptr_t)p);
    if (r) {
        if (!r->pending)
            s_mem_bytes -= r->size;
        r->state = 2;
    }
    free(p);
}

/* El motor libera memoria del PVR en medio del frame: el cache de sprites
 * (r_phase3.c) se vacia entero cuando se llena, mientras dibuja las cosas,
 * y las listas de este frame ya tienen headers apuntando a esos sprites.
 * En el Dreamcast el PVR lee VRAM que sigue ahi (a lo sumo un sprite con
 * basura un frame); aca las listas se reproducen recien en
 * pvr_scene_finish, y leer memoria ya liberada colgaba la consola (mapa 35
 * con muchos enemigos vivos). La liberacion real se hace despues de
 * reproducir la escena. */
#define MEM_DEFER_MAX 16384
static pvr_ptr_t s_mem_defer[MEM_DEFER_MAX];
static int       s_mem_ndefer = 0;

void pvr_mem_free(pvr_ptr_t p)
{
    if (!p) return;
    if (s_mem_ndefer < MEM_DEFER_MAX) {
        /* para el motor ya esta libre: cuenta como VRAM disponible */
        memrec_t *r = mem_find((uintptr_t)p);
        if (r && !r->pending) {
            s_mem_bytes -= r->size;
            r->pending = 1;
        }
        s_mem_defer[s_mem_ndefer++] = p;
        return;
    }
    mem_free_now(p);                  /* no deberia pasar nunca */
}

static void mem_flush_deferred(void)
{
    int i;
    for (i = 0; i < s_mem_ndefer; i++)
        mem_free_now(s_mem_defer[i]);
    s_mem_ndefer = 0;
}

uint32_t pvr_mem_available(void)
{
    return s_mem_bytes >= PVR_VRAM_BYTES ? 0 : PVR_VRAM_BYTES - s_mem_bytes;
}

static void mem_loaded(pvr_ptr_t dst, int linear)
{
    memrec_t *r = mem_find((uintptr_t)dst);
    tex_invalidate((uintptr_t)dst);
    if (r) {
        r->gen++;
        r->linear = (uint8_t)linear;
    }
}

void pvr_txr_load(void *src, pvr_ptr_t dst, uint32_t count)
{
    if (src && dst && count) memcpy(dst, src, count);
    mem_loaded(dst, 0);
}

void pvr_txr_load_ex(void *src, pvr_ptr_t dst, uint32_t w, uint32_t h, uint32_t flags)
{
    uint32_t bpp = (flags & PVR_TXRLOAD_32BPP) ? 4 : (flags & PVR_TXRLOAD_16BPP) ? 2 : 1;
    if (src && dst) memcpy(dst, src, w * h * bpp);
    mem_loaded(dst, 1);
}

/* Paletas: 1024 entradas (4 bancos de 256 para PAL8, 64 de 16 para PAL4). */
static uint32_t s_pal[1024];
static int      s_pal_fmt = PVR_PAL_ARGB1555;
static uint32_t s_pal_gen = 1;

/* Correccion de gamma (Opciones > Video > Gamma, como en Crispy Doom).
 * Se aplica a los texels al convertir cada textura y al color de cada
 * vertice. Como el RSX multiplica textura * color y (a*b)^g = a^g * b^g,
 * el resultado es la gamma exacta del pixel final (salvo el offset aditivo
 * y el blending, que quedan aproximados). */
static const float s_gamma_vals[PS3_GAMMA_LEVELS] = {
    0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f, 2.50f, 3.00f
};
static uint8_t s_gamma_lut[256];
static int     s_gamma_level = -1;
static uintptr_t s_raw_tex = 0;  /* textura sin gamma (captura para el melt/fade) */

void PS3_PVR_SetRawTexture(void *ptr) { s_raw_tex = (uintptr_t)ptr; }

void PS3_PVR_SetGamma(int level)
{
    int i;
    float inv;
    if (level < 0 || level >= PS3_GAMMA_LEVELS) level = PS3_GAMMA_OFF;
    if (level == s_gamma_level) return;
    s_gamma_level = level;
    inv = 1.0f / s_gamma_vals[level];
    for (i = 0; i < 256; i++) {
        int v = (int)(255.0f * powf((float)i / 255.0f, inv) + 0.5f);
        s_gamma_lut[i] = (uint8_t)(v > 255 ? 255 : v);
    }
    s_pal_gen++;                 /* todas las texturas se rehacen con la LUT nueva */
    ps3_logf("[video] gamma nivel %d (%.2f)", level, s_gamma_vals[level]);
}

void pvr_set_pal_format(int fmt)
{
    s_pal_fmt = fmt;
    s_pal_gen++;
}

void pvr_set_pal_entry(uint32_t i, uint32_t v)
{
    if (i < 1024 && s_pal[i] != v) {
        s_pal[i] = v;
        s_pal_gen++;
    }
}

/* ================================================================== */
/* Fog                                                                  */
/* ================================================================== */

static int   s_fog_valid = 0;
static int   s_fog_r = 0, s_fog_g = 0, s_fog_b = 0;
static float s_fog_start = 0.0f, s_fog_end = 1.0f;

void pvr_fog_table_color(float a, float r, float g, float b)
{
    (void)a;
    s_fog_r = (int)(r * 255.0f);
    s_fog_g = (int)(g * 255.0f);
    s_fog_b = (int)(b * 255.0f);
}

void pvr_fog_table_linear(float start, float end)
{
    /* El motor le pasa la mitad de la distancia (r_main.c: "* 0.5f"), que es
     * lo que espera la tabla de fog de KOS. Aca la fog se calcula con w
     * directo, asi que se deshace. Si el fog se ve muy cerca o muy lejos,
     * es este factor. */
    s_fog_start = start * 2.0f;
    s_fog_end   = end   * 2.0f;
    if (s_fog_end <= s_fog_start) s_fog_end = s_fog_start + 1.0f;
    s_fog_valid = 1;
}

void pvr_fog_table_custom(float t[]) { (void)t; }

uint32_t pvr_pack_bump(float h, float t, float q)
{ (void)h; (void)t; (void)q; return 0; }

/* ================================================================== */
/* Cache de texturas: memoria del PVR -> textura del RSX                */
/* ================================================================== */

#define TEXC_MAX   8192                      /* potencia de 2 */

typedef struct {
    uintptr_t   ptr;
    uint32_t    fmt;
    uint16_t    w, h;
    uint32_t    gen, pal_gen;
    ps3_rtex_t *tex;
    uint8_t     state;                       /* 0 libre, 1 usado, 2 borrado */
} texent_t;

static texent_t s_texc[TEXC_MAX];
static int s_tex_live = 0, s_tex_created = 0, s_tex_failed = 0;

static void tex_invalidate(uintptr_t p)
{
    int i;
    if (!s_tex_live) return;
    for (i = 0; i < TEXC_MAX; i++) {
        texent_t *e = &s_texc[i];
        if (e->state == 1 && e->ptr == p) {
            if (e->tex) PS3_RSX_TexRelease(e->tex);
            e->tex = NULL;
            e->state = 2;
            s_tex_live--;
        }
    }
}

/* Indice twiddled (Morton) del texel (x,y), como lo arma el PVR. Para
 * texturas no cuadradas son bloques cuadrados de min(w,h) uno detras del
 * otro. Dentro de cada bloque, el bit 0 del indice es el bit 0 de y, el
 * bit 1 el bit 0 de x, y asi alternando. */
static uint32_t twiddle_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t m = w < h ? w : h;
    uint32_t base, idx = 0, bit;

    if (w > h) { base = (x / m) * m * m; x %= m; }
    else       { base = (y / m) * m * m; y %= m; }

    for (bit = 0; (1u << bit) < m; bit++) {
        idx |= ((y >> bit) & 1u) << (2 * bit);
        idx |= ((x >> bit) & 1u) << (2 * bit + 1);
    }
    return base + idx;
}

static uint32_t argb1555_to_8888(uint32_t v)
{
    uint32_t a = (v & 0x8000) ? 0xff : 0;
    uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t rgb565_to_8888(uint32_t v)
{
    uint32_t r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static uint32_t argb4444_to_8888(uint32_t v)
{
    uint32_t a = (v >> 12) & 0xf, r = (v >> 8) & 0xf, g = (v >> 4) & 0xf, b = v & 0xf;
    return ((a * 17) << 24) | ((r * 17) << 16) | ((g * 17) << 8) | (b * 17);
}

static uint32_t pal_to_8888(uint32_t i)
{
    uint32_t v = s_pal[i & 1023];
    switch (s_pal_fmt) {
    case PVR_PAL_RGB565:   return rgb565_to_8888(v);
    case PVR_PAL_ARGB4444: return argb4444_to_8888(v);
    case PVR_PAL_ARGB8888: return v;
    default:               return argb1555_to_8888(v);
    }
}

/* Convierte la textura del PVR a 0xAARRGGBB lineal. 0 si el formato no se
 * soporta (bump). Lee con limite: si el bloque es mas chico que lo que
 * dice el header, lo que falta queda transparente. */
static int convert_texture(const uint8_t *src, uint32_t avail, uint32_t fmt,
                           int w, int h, int linear, uint32_t *out)
{
    uint32_t pix = (fmt >> 27) & 7;
    int paletted = (pix == 5 || pix == 6);
    /* En los formatos con paleta el bit 26 es parte del banco, no el flag de
     * "no twiddled" (en el PVR las texturas con paleta van siempre
     * twiddled). */
    int twiddled = !linear && (paletted || !(fmt & PVR_TXRFMT_NONTWIDDLED));
    int x, y;

    if (pix == 4 || pix == 7) return 0;                    /* bump / YUV */

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t idx = twiddled ? twiddle_index((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h)
                                    : (uint32_t)(y * w + x);
            uint32_t c = 0;

            switch (pix) {
            case 0: case 1: case 2: case 3: {                /* 16 bpp */
                uint32_t v;
                if (idx * 2 + 1 >= avail) break;
                v = ((const uint16_t *)src)[idx];
                c = (pix == 1) ? rgb565_to_8888(v)
                  : (pix == 2) ? argb4444_to_8888(v)
                  :              argb1555_to_8888(v);
                break;
            }
            case 6: {                                        /* PAL8 */
                uint32_t bank = (fmt >> 25) & 3;
                if (idx >= avail) break;
                c = pal_to_8888(bank * 256 + src[idx]);
                break;
            }
            case 5: {                                        /* PAL4 */
                uint32_t bank = (fmt >> 21) & 0x3f;
                uint32_t nib;
                if ((idx >> 1) >= avail) break;
                nib = (idx & 1) ? (src[idx >> 1] >> 4) : (src[idx >> 1] & 0xf);
                c = pal_to_8888(bank * 16 + nib);
                break;
            }
            default:
                break;
            }
            out[y * w + x] = c;
        }
    }
    return 1;
}

static int s_texlog = 0;            /* texturas logueadas al crearse */

static ps3_rtex_t *get_texture(uintptr_t ptr, uint32_t fmt, int w, int h)
{
    memrec_t *r;
    uint32_t gen, hsh, i, avail;
    texent_t *slot = NULL;
    uint32_t *buf;
    ps3_rtex_t *t;
    int linear;

    if (!ptr || w <= 0 || h <= 0 || w > 1024 || h > 1024) return NULL;

    r = mem_find(ptr);
    gen = r ? r->gen : 0;
    linear = r ? r->linear : 0;
    avail = r ? r->size : (uint32_t)(w * h * 2);

    hsh = ptr_hash(ptr) ^ (fmt * 31u) ^ ((uint32_t)w << 16) ^ (uint32_t)h;
    for (i = 0; i < TEXC_MAX; i++) {
        texent_t *e = &s_texc[(hsh + i) & (TEXC_MAX - 1)];
        if (e->state == 0) { if (!slot) slot = e; break; }
        if (e->state == 2) { if (!slot) slot = e; continue; }
        if (e->ptr == ptr && e->fmt == fmt && e->w == w && e->h == h) {
            if (e->gen == gen && e->pal_gen == s_pal_gen)
                return e->tex;
            /* contenido o paleta nuevos: se rehace en este mismo lugar */
            if (e->tex) PS3_RSX_TexRelease(e->tex);
            e->tex = NULL;
            e->state = 2;
            s_tex_live--;
            if (!slot) slot = e;
        }
    }
    if (!slot) return NULL;                                  /* cache lleno */

    buf = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!buf) return NULL;
    if (!convert_texture((const uint8_t *)ptr, avail, fmt, w, h, linear, buf)) {
        free(buf);
        return NULL;
    }
    /* La captura de pantalla del melt ya salio con gamma: aplicarla de nuevo
     * (y el melt recaptura ~150 veces) la llevaba a blanco total. */
    if (s_gamma_level != PS3_GAMMA_OFF && s_gamma_level >= 0 && ptr != s_raw_tex) {
        uint32_t k, n = (uint32_t)(w * h);
        for (k = 0; k < n; k++) {
            uint32_t c = buf[k];
            buf[k] = (c & 0xff000000u) |
                     ((uint32_t)s_gamma_lut[(c >> 16) & 0xff] << 16) |
                     ((uint32_t)s_gamma_lut[(c >>  8) & 0xff] << 8) |
                      (uint32_t)s_gamma_lut[ c        & 0xff];
        }
    }
#ifdef PS3_HOST_DEBUG
    {
        static int dumped = 0;
        if (dumped < 12) {
            char path[64]; FILE *f; int k;
            snprintf(path, sizeof(path), "/tmp/ltest/conv_%02d_%dx%d_%08x.ppm", dumped, w, h, (unsigned)fmt);
            f = fopen(path, "wb");
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (k = 0; k < w * h; k++) { fputc((buf[k] >> 16) & 255, f); fputc((buf[k] >> 8) & 255, f); fputc(buf[k] & 255, f); }
            fclose(f);
            dumped++;
        }
    }
#endif
    if (s_texlog < 0) {                 /* (log de texturas apagado para la 1.0) */
        int k, a0 = 0, a255 = 0, n = w * h;
        for (k = 0; k < n; k++) {
            uint32_t a = buf[k] >> 24;
            if (a == 0) a0++; else if (a == 255) a255++;
        }
        ps3_logf("[tex] #%d ptr=%p fmt=%08x %dx%d %s  alfa0=%d%% alfa255=%d%%  texel0=%08x texelMedio=%08x",
                 s_texlog, (void *)ptr, (unsigned)fmt, w, h, linear ? "lineal" : "twiddled",
                 a0 * 100 / n, a255 * 100 / n, (unsigned)buf[0], (unsigned)buf[n / 2 + w / 2]);
        s_texlog++;
    }
    t = PS3_RSX_Ready() ? PS3_RSX_TexCreate(w, h, buf) : NULL;

    free(buf);
    if (!t) { s_tex_failed++; return NULL; }

    slot->ptr = ptr; slot->fmt = fmt; slot->w = (uint16_t)w; slot->h = (uint16_t)h;
    slot->gen = gen; slot->pal_gen = s_pal_gen; slot->tex = t; slot->state = 1;
    s_tex_live++;
    s_tex_created++;
    return t;
}

/* ================================================================== */
/* Replay: del stream del TA a triangulos del RSX                        */
/* ================================================================== */

typedef struct {
    int      valid;
    int      sprite;
    int      list;
    uint32_t fmt;
    int      specular, galpha, fog;
    uintptr_t txr;
    uint32_t sprite_argb, sprite_oargb;
    int      skip;              /* bump maps: no se dibujan */
    int      textured;          /* hay textura RSX para este header */
    int      env;               /* PVR_TXRENV_* */
    uint32_t tint;              /* sin textura: gris por textura, 0..256 */
    ps3_rstate_t rs;
} hdr_state_t;

#define BATCH_MAX   (3 * 4096)
static ps3_rvert_t s_batch[BATCH_MAX] __attribute__((aligned(16)));
static int         s_batch_n = 0;
static ps3_rstate_t s_batch_rs;

typedef struct {
    int records, headers, sprite_hdrs, sprites, strips, tris, skipped, unknown;
} list_stats_t;
static list_stats_t s_stats[NUM_LISTS];
static int s_clipped = 0;           /* triangulos recortados contra la pantalla */
static int s_badtris = 0;           /* triangulos descartados por NaN/Inf/z<=0 */
static int s_badlog = 0;
static int s_clipx_left = 0;        /* triangulos recortados por loguear */
static int s_diag_left = 0;         /* tiras que quedan por loguear */
extern int g_rsx_dbg;
const char *PS3_RSX_DbgName(int m);
extern int g_rsx_drawlog;
void PS3_Sys_Overlay(void);
void PS3_PVR_DiagNext(void) { s_diag_left = 14; g_rsx_drawlog = 80; s_clipx_left = 8; }

/* Texturas activadas. Se pueden apagar con un archivo NOTEX en USRDIR (sin
 * recompilar), para aislar un problema del renderer: si con NOTEX anda y
 * sin el no, el problema esta en las texturas. */
static int s_textures_on = 1;

void PS3_PVR_SetTextures(int on) { s_textures_on = on; }

static void batch_flush(void)
{
    if (s_batch_n > 0 && PS3_RSX_Ready())
        PS3_RSX_DrawTris(&s_batch_rs, s_batch, s_batch_n);
    s_batch_n = 0;
}

static void batch_state(const ps3_rstate_t *rs)
{
    if (memcmp(rs, &s_batch_rs, sizeof(*rs)) != 0) {
        batch_flush();
        s_batch_rs = *rs;
    }
}

/* PVR compara 1/w (mayor = mas cerca). El depth buffer del RSX guarda
 * d = 1/(1 + 8/w) (menor = mas cerca). Las comparaciones se dan vuelta. */
static uint8_t depth_map(int pvr_cmp)
{
    switch (pvr_cmp) {
    case PVR_DEPTHCMP_NEVER:   return RSX_DEPTH_NEVER;
    case PVR_DEPTHCMP_LESS:    return RSX_DEPTH_GREATER;
    case PVR_DEPTHCMP_EQUAL:   return RSX_DEPTH_EQUAL;
    case PVR_DEPTHCMP_LEQUAL:  return RSX_DEPTH_GEQUAL;
    case PVR_DEPTHCMP_GREATER: return RSX_DEPTH_LESS;
    case 5:                    return RSX_DEPTH_NOTEQUAL;
    case PVR_DEPTHCMP_GEQUAL:  return RSX_DEPTH_LEQUAL;
    default:                   return RSX_DEPTH_ALWAYS;
    }
}

static uint32_t hash_tint(uintptr_t p)
{
    uint32_t h = (uint32_t)(p >> 5) ^ (uint32_t)((uint64_t)p >> 32);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return 150 + (h % 107);                    /* 150..256 de 256 */
}

static void parse_header(hdr_state_t *st, const uint8_t *rec, int list)
{
    const pvr_poly_hdr_t *h = (const pvr_poly_hdr_t *)rec;
    uint32_t cmd = h->cmd, m1 = h->mode1, m2 = h->mode2, m3 = h->mode3;
    int src, dst, txr;

    st->valid    = 1;
    st->sprite   = (cmd >> 29) == 5;
    st->list     = list;
    st->fmt      = m3 & 0xffe00000u;
    st->specular = (cmd >> 2) & 1;
    st->galpha   = (m2 >> 20) & 1;
    st->fog      = (m2 >> 22) & 3;
    st->env      = (int)((m2 >> 6) & 3);
    st->skip     = 0;
    txr          = (cmd >> 3) & 1;

    if (st->sprite) {
        /* En el header de sprite el puntero esta en otro lugar: argb/oargb
         * ocupan lo que en el de poligono son d1/d2. */
        const pvr_sprite_hdr_t *sh = (const pvr_sprite_hdr_t *)rec;
        st->sprite_argb  = sh->argb;
        st->sprite_oargb = sh->oargb;
        st->txr = (uintptr_t)(((uint64_t)sh->d1 << 32) | sh->d2);
    } else {
        st->txr = (uintptr_t)(((uint64_t)h->d1 << 32) | h->d2);
        /* Un header que no compilamos nosotros (memoria en cero, por ej.). */
        if (h->d4 != HDR_SIGNATURE)
            st->skip = 1;
    }

    /* Bump maps (formato BUMP): en el Dreamcast son una pasada aparte que
     * despues se multiplica. Sin ellos se dibujaria negro. Afuera. */
    if (txr && (st->fmt & (7u << 27)) == PVR_TXRFMT_BUMP)
        st->skip = 1;

    /* Pasada que LEE el buffer secundario del PVR (blend src_enable, bit 25):
     * en modo Ultra el arma se arma como bump -> diffuse*bump escrito en el
     * buffer secundario -> "flush" que lo copia a pantalla. El buffer
     * secundario no esta emulado: esa copia salia como un cuadrado solido
     * detras del arma. Se descarta; el diffuse (dst_enable) se dibuja
     * directo en pantalla, abajo. */
    if ((m2 >> 25) & 1)
        st->skip = 1;

    memset(&st->rs, 0, sizeof(st->rs));

    /* Textura: se convierte (o se toma del cache) al leer el header. Si no
     * se puede, el poligono se dibuja con un gris derivado de la textura,
     * como en la etapa 3a. */
    st->textured = 0;
    st->tint = 256;
    if (txr && !st->skip) {
        int w  = 8 << ((m2 >> 3) & 7);
        int hh = 8 << (m2 & 7);
        ps3_rtex_t *t = s_textures_on ? get_texture(st->txr, st->fmt, w, hh) : NULL;
        if (t) {
            st->textured      = 1;
            st->rs.tex        = t;
            st->rs.alpha_test = 1;       /* el color 0 de la paleta es transparente */
            st->rs.filter     = ((m2 >> 12) & 7) ? 1 : 0;
            /* clamp: bit 16 = U, bit 15 = V. Espejado (repeticion en espejo,
             * el "mirror" de las texturas del N64): bit 18 = U, bit 17 = V.
             * rs.clamp: bit0/1 = clamp U/V, bit2/3 = espejo U/V. */
            st->rs.clamp      = (uint8_t)(((m2 >> 16) & 1) | (((m2 >> 15) & 1) << 1) |
                                          (((m2 >> 18) & 1) << 2) | (((m2 >> 17) & 1) << 3));
            if (st->sprite) st->rs.clamp = 3;
        } else {
            st->tint = hash_tint(st->txr);
        }
    }

    st->rs.depth_func  = depth_map((int)((m1 >> 29) & 7));
    /* Sprites del HUD y los menus: van a la misma z y se tapan en orden
     * (barra del slider y despues la perilla). En el PVR la lista TR se
     * dibuja por orden de llegada; en el RSX con LESS el segundo pierde. */
    if (st->sprite && st->rs.depth_func == RSX_DEPTH_LESS)
        st->rs.depth_func = RSX_DEPTH_LEQUAL;
    st->rs.depth_write = ((m1 >> 26) & 1) ? 0 : 1;     /* 1 = DISABLE en el PVR */

    src = (int)((m2 >> 29) & 7);
    dst = (int)((m2 >> 26) & 7);

    if (list == PVR_LIST_TR_POLY) {
        /* DESTCOLOR*ZERO es "multiplicar sobre la pasada de bump", que no
         * existe todavia: se dibuja opaco. */
        if ((src == PVR_BLEND_DESTCOLOR && dst == PVR_BLEND_ZERO) ||
            (src == PVR_BLEND_ONE && dst == PVR_BLEND_ZERO)) {
            st->rs.blend = 0;
        } else {
            st->rs.blend = 1;
            st->rs.src = (uint8_t)src;
            st->rs.dst = (uint8_t)dst;
        }
    }
    /* OP y PT: sin blending (PT se resuelve con el alpha test). */
}

static inline uint32_t sat_add(uint32_t a, uint32_t b)
{
    uint32_t s = a + b;
    return s > 255 ? 255 : s;
}

/* Color final de un vertice.
 *
 * El PVR hace  texel * argb + oargb  (con "specular" = color de offset). El
 * fragment program que tenemos precompilado solo hace  color * texel, asi
 * que el offset se suma al color ANTES de multiplicar: tex*(argb+oargb).
 * Con los valores que usa este motor la diferencia es chica.
 *
 * Segun el modo de textura:
 *   REPLACE      el texel tal cual  -> color blanco
 *   MODULATE     texel * color, alfa del texel
 *   DECAL        (solo lo usan los bump maps, que no se dibujan) -> blanco
 *   MODULATEALPHA texel * color, alfa del texel * alfa del vertice */
static uint32_t vertex_rgba(const hdr_state_t *st, uint32_t argb, uint32_t oargb, float w)
{
    uint32_t a = (argb >> 24) & 0xff;
    uint32_t r = (argb >> 16) & 0xff;
    uint32_t g = (argb >>  8) & 0xff;
    uint32_t b =  argb        & 0xff;
    uint32_t fog;

    if (st->textured && (st->env == PVR_TXRENV_REPLACE || st->env == PVR_TXRENV_DECAL)) {
        r = g = b = 255;
        a = 255;
    } else {
        r = (r * st->tint) >> 8;
        g = (g * st->tint) >> 8;
        b = (b * st->tint) >> 8;
    }
    if (st->textured && st->env == PVR_TXRENV_MODULATE)
        a = 255;

    if (st->specular) {
        r = sat_add(r, (oargb >> 16) & 0xff);
        g = sat_add(g, (oargb >>  8) & 0xff);
        b = sat_add(b,  oargb        & 0xff);
    }

    fog = (uint32_t)st->fog;
    if (s_fog_valid && (fog == PVR_FOG_TABLE || fog == PVR_FOG_TABLE2)) {
        float f = (w - s_fog_start) / (s_fog_end - s_fog_start);
        if (f > 0.0f) {
            int k;
            if (f > 1.0f) f = 1.0f;
            k = (int)(f * 256.0f);
            r = (r * (256 - k) + (uint32_t)s_fog_r * k) >> 8;
            g = (g * (256 - k) + (uint32_t)s_fog_g * k) >> 8;
            b = (b * (256 - k) + (uint32_t)s_fog_b * k) >> 8;
        }
    }

    if (!st->galpha || st->list != PVR_LIST_TR_POLY)
        a = 255;

    if (s_gamma_level != PS3_GAMMA_OFF && s_gamma_level >= 0) {
        r = s_gamma_lut[r > 255 ? 255 : r];
        g = s_gamma_lut[g > 255 ? 255 : g];
        b = s_gamma_lut[b > 255 ? 255 : b];
    }

    return (r << 24) | (g << 16) | (b << 8) | a;
}

/* Pixel del PVR (640x480, z = 1/w) -> clip space del RSX.
 *   w = 1/z                  -> perspectiva correcta al interpolar
 *   x,y a NDC [-1,1], multiplicados por w (el RSX divide despues)
 *   profundidad d = 1/(1+8z) en (0,1], menor = mas cerca; NDC = 2d-1
 * Los elementos 2D (HUD, lineas del automapa) usan z constante, y con esta
 * formula siempre caen dentro del rango. */
static void convert_vertex(const hdr_state_t *st, const pvr_vertex_t *v, ps3_rvert_t *o)
{
    float iz = v->z;
    float w, d;

    if (iz < 1e-6f) iz = 1e-6f;
    w = 1.0f / iz;
    d = 1.0f / (1.0f + 8.0f * iz);

    /* Clip space con la w real (= 1/iz): el vertex program de IoQuake3 hace
     * mul(mvp, float4 pos) con mvp identidad, asi que la w llega intacta al
     * rasterizador y el RSX interpola uv/color con correccion de perspectiva.
     * Con w=1 las texturas quedaban afines ("swirl" en paredes de costado).
     * El recorte 2D ya dejo todo dentro de la pantalla, asi que el RSX no
     * tiene nada que recortar. */
    o->x  = (v->x * (2.0f / RSX_GAME_W) - 1.0f) * w;
    o->y  = (1.0f - v->y * (2.0f / RSX_GAME_H)) * w;
    o->z  = (2.0f * d - 1.0f) * w;
    o->w  = w;
    o->u  = v->u;
    o->v  = v->v;
    o->u1 = 0.0f;
    o->v1 = 0.0f;
    o->rgba = vertex_rgba(st, v->argb, v->oargb, w);
}

/* ------------------------------------------------------------------ */
/* Recorte 2D contra la pantalla                                        */
/*                                                                      */
/* Los vertices llegan ya proyectados. Los de un piso o pared pegados a */
/* la camara pueden caer a miles de pixeles fuera de la pantalla. El    */
/* PVR (y un rasterizador por software) se lo banca; el RSX tiene un    */
/* "guard band" limitado y, pasado ese limite, el triangulo se desborda */
/* y tapa toda la pantalla (sintoma en hardware: todo del color del     */
/* piso, con el HUD y el automapa bien). Asi que cada triangulo que se  */
/* sale se recorta aca contra un rectangulo apenas mas grande que la    */
/* pantalla, interpolando uv y colores con correccion de perspectiva.   */
/* ------------------------------------------------------------------ */

/* Pista del hardware (modo 9 de R3): los triangulos TEXTURIZADOS que se
 * salen del viewport salen negros en el RSX; los que caen adentro, bien.
 * Asi que se recorta un cuarto de pixel ADENTRO de la pantalla: nada
 * texturizado llega a salirse. Modo 11 de R3 = margen viejo (+32 px). */
extern int g_rsx_dbg;
#define CLIP_MARGIN (g_rsx_dbg == 11 ? 32.0f : -0.25f)
#define CLIP_XMIN  (0.0f - CLIP_MARGIN)
#define CLIP_XMAX  (RSX_GAME_W + CLIP_MARGIN)
#define CLIP_YMIN  (0.0f - CLIP_MARGIN)
#define CLIP_YMAX  (RSX_GAME_H + CLIP_MARGIN)

typedef struct { float x, y, iz, u, v, c[8]; } cvert_t;

static void to_cvert(const pvr_vertex_t *p, cvert_t *o)
{
    int k;
    o->x = p->x; o->y = p->y; o->iz = p->z; o->u = p->u; o->v = p->v;
    for (k = 0; k < 4; k++) {
        o->c[k]     = (float)((p->argb  >> (24 - 8 * k)) & 0xff);
        o->c[4 + k] = (float)((p->oargb >> (24 - 8 * k)) & 0xff);
    }
}

static void from_cvert(const cvert_t *c, pvr_vertex_t *p)
{
    uint32_t a = 0, o = 0;
    int k;
    for (k = 0; k < 4; k++) {
        int x = (int)(c->c[k] + 0.5f), y = (int)(c->c[4 + k] + 0.5f);
        x = x < 0 ? 0 : (x > 255 ? 255 : x);
        y = y < 0 ? 0 : (y > 255 ? 255 : y);
        a |= (uint32_t)x << (24 - 8 * k);
        o |= (uint32_t)y << (24 - 8 * k);
    }
    p->flags = PVR_CMD_VERTEX;
    p->x = c->x; p->y = c->y; p->z = c->iz; p->u = c->u; p->v = c->v;
    p->argb = a; p->oargb = o;
}

/* Punto en t del segmento a->b (t en espacio de pantalla). Los atributos
 * se interpolan pesados por 1/w para que queden perspectiva-correctos. */
static void cvert_lerp(const cvert_t *a, const cvert_t *b, float t, cvert_t *o)
{
    float wa = a->iz * (1.0f - t), wb = b->iz * t, s = wa + wb;
    int k;
    if (s < 1e-12f) s = 1e-12f;
    o->x  = a->x  + (b->x  - a->x)  * t;
    o->y  = a->y  + (b->y  - a->y)  * t;
    o->iz = a->iz + (b->iz - a->iz) * t;
    o->u  = (a->u * wa + b->u * wb) / s;
    o->v  = (a->v * wa + b->v * wb) / s;
    for (k = 0; k < 8; k++)
        o->c[k] = (a->c[k] * wa + b->c[k] * wb) / s;
}

/* Sutherland-Hodgman contra un borde. axis 0 = x, 1 = y; sign +1 = "<= lim". */
static int clip_edge(const cvert_t *in, int n, cvert_t *out, int axis, float lim, float sign)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const cvert_t *a = &in[i], *b = &in[(i + 1) % n];
        float da = sign * ((axis ? a->y : a->x) - lim);
        float db = sign * ((axis ? b->y : b->x) - lim);
        if (da <= 0.0f) out[m++] = *a;
        if ((da <= 0.0f) != (db <= 0.0f)) {
            float t = da / (da - db);
            cvert_lerp(a, b, t, &out[m++]);
        }
    }
    return m;
}

static int fin(float f) { return f == f && f < 3.0e38f && f > -3.0e38f; }
static int vert_ok(const pvr_vertex_t *v)
{
    return fin(v->x) && fin(v->y) && fin(v->z) && v->z > 0.0f &&
           fin(v->u) && fin(v->v) &&
           v->x > -1.0e6f && v->x < 1.0e6f && v->y > -1.0e6f && v->y < 1.0e6f;
}

static int tri_inside(const pvr_vertex_t *a, const pvr_vertex_t *b, const pvr_vertex_t *c)
{
    return a->x >= CLIP_XMIN && a->x <= CLIP_XMAX && a->y >= CLIP_YMIN && a->y <= CLIP_YMAX &&
           b->x >= CLIP_XMIN && b->x <= CLIP_XMAX && b->y >= CLIP_YMIN && b->y <= CLIP_YMAX &&
           c->x >= CLIP_XMIN && c->x <= CLIP_XMAX && c->y >= CLIP_YMIN && c->y <= CLIP_YMAX;
}

static void push_tri(const ps3_rvert_t *a, const ps3_rvert_t *b, const ps3_rvert_t *c,
                     list_stats_t *ls)
{
    if (s_batch_n + 3 > BATCH_MAX) batch_flush();
    s_batch[s_batch_n++] = *a;
    s_batch[s_batch_n++] = *b;
    s_batch[s_batch_n++] = *c;
    ls->tris++;
}

/* Un triangulo del PVR -> uno o varios triangulos RSX, recortados. */
static void emit_tri(hdr_state_t *st, const pvr_vertex_t *a, const pvr_vertex_t *b,
                     const pvr_vertex_t *c, list_stats_t *ls)
{
    cvert_t p0[12], p1[12];
    ps3_rvert_t rv[12];
    pvr_vertex_t pv;
    int n, i;

    /* Un vertice con NaN/Inf (o z <= 0: detras de la camara) arma en el RSX
     * un triangulo que tapa TODA la pantalla. Afuera. */
    if (!vert_ok(a) || !vert_ok(b) || !vert_ok(c)) {
        if (s_badlog < 12) {
            const pvr_vertex_t *q = !vert_ok(a) ? a : !vert_ok(b) ? b : c;
            ps3_logf("[bad] frame %d: vertice invalido (%g, %g, z=%g) uv(%g, %g)",
                     s_frame, q->x, q->y, q->z, q->u, q->v);
            s_badlog++;
        }
        s_badtris++;
        return;
    }
    if (g_rsx_dbg == 9 && !tri_inside(a, b, c)) return;   /* diag: sin recortados */
    if (g_rsx_dbg == 13 && tri_inside(a, b, c)) return;   /* diag: SOLO recortados */
    if (tri_inside(a, b, c)) {
        convert_vertex(st, a, &rv[0]);
        convert_vertex(st, b, &rv[1]);
        convert_vertex(st, c, &rv[2]);
        push_tri(&rv[0], &rv[1], &rv[2], ls);
        return;
    }

    to_cvert(a, &p0[0]); to_cvert(b, &p0[1]); to_cvert(c, &p0[2]);
    n = 3;
    n = clip_edge(p0, n, p1, 0, CLIP_XMIN, -1.0f); if (n < 3) return;
    n = clip_edge(p1, n, p0, 0, CLIP_XMAX, +1.0f); if (n < 3) return;
    n = clip_edge(p0, n, p1, 1, CLIP_YMIN, -1.0f); if (n < 3) return;
    n = clip_edge(p1, n, p0, 1, CLIP_YMAX, +1.0f); if (n < 3) return;

    for (i = 0; i < n; i++) {
        from_cvert(&p0[i], &pv);
        convert_vertex(st, &pv, &rv[i]);
    }
    if (s_clipx_left > 0) {
        s_clipx_left--;
        ps3_logf("[clipX] in a(%.1f,%.1f z=%.5f uv %.3f,%.3f) b(%.1f,%.1f z=%.5f uv %.3f,%.3f) c(%.1f,%.1f z=%.5f uv %.3f,%.3f) -> %d verts",
                 a->x, a->y, a->z, a->u, a->v, b->x, b->y, b->z, b->u, b->v,
                 c->x, c->y, c->z, c->u, c->v, n);
        for (i = 0; i < n; i++)
            ps3_logf("[clipX]   ndc(%.3f,%.3f,%.5f) uv(%.3f,%.3f) rgba=%08x",
                     rv[i].x, rv[i].y, rv[i].z, rv[i].u, rv[i].v, (unsigned)rv[i].rgba);
    }
    if (g_rsx_dbg == 12) {                   /* diag: recortados sin textura */
        ps3_rstate_t plain = st->rs;
        plain.tex = NULL; plain.alpha_test = 0;
        batch_state(&plain);
        for (i = 1; i + 1 < n; i++)
            push_tri(&rv[0], &rv[i], &rv[i + 1], ls);
        batch_state(&st->rs);
    } else {
        for (i = 1; i + 1 < n; i++)
            push_tri(&rv[0], &rv[i], &rv[i + 1], ls);
    }
    s_clipped++;
}

static void emit_strip(hdr_state_t *st, const pvr_vertex_t **sv, int n, list_stats_t *ls)
{
    int i;

    ls->strips++;
    if (n < 3 || st->skip || !st->valid) {
        if (st->skip) ls->skipped++;
        return;
    }

    /* diag: sin cielo (tira texturizada con todos los vertices muy lejos) */
    if ((g_rsx_dbg == 7 || g_rsx_dbg == 10) && st->textured) {
        int far = 1;
        for (i = 0; i < n; i++) if (sv[i]->z >= 0.0005f) { far = 0; break; }
        if (far) return;
    }

    /* Diagnostico: muestra de tiras (las primeras 4 y una de cada 20) en el
     * frame 1 y en el frame siguiente a cada cambio de modo con R3. */
    if (s_diag_left > 0 && (ls->strips <= 4 || (ls->strips % 20) == 0)) {
        ps3_rvert_t cv;
        s_diag_left--;
        ps3_logf("[diag] %s tira %d: %d verts depth=%d wr=%d blend=%d(%d,%d) atest=%d "
                 "tex=%d(%p) env=%d fog=%d galpha=%d clamp=%d filt=%d fmt=%08x",
                 st->list == PVR_LIST_OP_POLY ? "OP" : st->list == PVR_LIST_PT_POLY ? "PT" : "TR",
                 ls->strips, n, st->rs.depth_func, st->rs.depth_write,
                 st->rs.blend, st->rs.src, st->rs.dst, st->rs.alpha_test,
                 st->textured, (void *)st->rs.tex, st->env, st->fog, st->galpha,
                 st->rs.clamp, st->rs.filter, (unsigned)st->fmt);
        for (i = 0; i < n && i < 3; i++) {
            convert_vertex(st, sv[i], &cv);
            ps3_logf("[diag]   pvr(%.1f,%.1f z=%.6f) uv(%.4f,%.4f) argb=%08x oargb=%08x -> ndc(%.3f,%.3f,%.5f) rgba=%08x",
                     sv[i]->x, sv[i]->y, sv[i]->z, sv[i]->u, sv[i]->v,
                     (unsigned)sv[i]->argb, (unsigned)sv[i]->oargb,
                     cv.x, cv.y, cv.z, (unsigned)cv.rgba);
        }
    }

    batch_state(&st->rs);

    /* Tira -> triangulos. El orden de vuelta alterna, pero el culling esta
     * apagado, asi que no importa. */
    for (i = 2; i < n; i++)
        emit_tri(st, sv[i - 2], sv[i - 1], sv[i], ls);
}

/* Coordenada UV empaquetada del PVR: los 16 bits altos de cada float. */
static float unpack_u(uint32_t uv) { union { uint32_t i; float f; } c; c.i = uv & 0xffff0000u; return c.f; }
static float unpack_v(uint32_t uv) { union { uint32_t i; float f; } c; c.i = uv << 16;          return c.f; }

/* Sprite 2D del PVR (HUD, fuentes, fondos de menu): cuatro esquinas a, b, c
 * y d, donde d no trae z ni uv (se deducen: es un paralelogramo). El color
 * sale del header, no de los vertices. */
static void emit_sprite(hdr_state_t *st, const pvr_sprite_txr_t *sp, list_stats_t *ls)
{
    pvr_vertex_t pv[4];
    ps3_rvert_t  cv[4];
    int i;
    float au = unpack_u(sp->auv), av = unpack_v(sp->auv);
    float bu = unpack_u(sp->buv), bv = unpack_v(sp->buv);
    float cu = unpack_u(sp->cuv), cvv = unpack_v(sp->cuv);

    ls->sprites++;
    if (st->skip || !st->valid) return;
    if (g_rsx_dbg == 8 || g_rsx_dbg == 10) return;          /* diag: sin sprites */

    memset(pv, 0, sizeof(pv));
    pv[0].x = sp->ax; pv[0].y = sp->ay; pv[0].z = sp->az; pv[0].u = au; pv[0].v = av;
    pv[1].x = sp->bx; pv[1].y = sp->by; pv[1].z = sp->bz; pv[1].u = bu; pv[1].v = bv;
    pv[2].x = sp->cx; pv[2].y = sp->cy; pv[2].z = sp->cz; pv[2].u = cu; pv[2].v = cvv;
    pv[3].x = sp->dx; pv[3].y = sp->dy; pv[3].z = sp->az;
    pv[3].u = au + cu - bu;
    pv[3].v = av + cvv - bv;
    for (i = 0; i < 4; i++) {
        pv[i].argb  = st->sprite_argb;
        pv[i].oargb = st->sprite_oargb;
    }
    (void)cv;

    /* Tambien pasan por el recorte: un sprite pegado al borde no se sale. */
    batch_state(&st->rs);
    emit_tri(st, &pv[0], &pv[1], &pv[2], ls);
    emit_tri(st, &pv[0], &pv[2], &pv[3], ls);
}

static void replay_list(int l)
{
    ta_list_t    *L = &s_list[l];
    list_stats_t *ls = &s_stats[l];
    hdr_state_t   st;
    const pvr_vertex_t *strip[64];
    int  nstrip = 0;
    uint32_t off = 0;

    memset(&st, 0, sizeof(st));
    memset(ls, 0, sizeof(*ls));
    if (!L->buf || L->len == 0) return;

    while (off + 32 <= L->len) {
        const uint8_t *rec = L->buf + off;
        uint32_t cmd = *(const uint32_t *)rec;
        uint32_t top = cmd >> 29;

        ls->records++;

        if (top == 4 || top == 5) {                     /* header */
            nstrip = 0;
            parse_header(&st, rec, l);
            if (top == 4) ls->headers++; else ls->sprite_hdrs++;
            off += 32;
            continue;
        }

        if (top == 7) {                                 /* vertice */
            if (st.sprite) {
                /* pvr_sprite_txr_t: 64 bytes */
                if (off + 64 <= L->len)
                    emit_sprite(&st, (const pvr_sprite_txr_t *)rec, ls);
                off += 64;
                continue;
            }
            strip[nstrip++] = (const pvr_vertex_t *)rec;
            if ((cmd & 0xf0000000u) == 0xf0000000u) {   /* EOL */
                emit_strip(&st, strip, nstrip, ls);
                nstrip = 0;
            } else if (nstrip == 64) {
                /* Tira mas larga que el buffer: se dibuja lo que hay y se
                 * sigue con los dos ultimos vertices (una tira continua). */
                emit_strip(&st, strip, nstrip, ls);
                strip[0] = strip[62];
                strip[1] = strip[63];
                nstrip = 2;
            }
            off += 32;
            continue;
        }

        if (top != 1) {                                 /* 1 = user clip */
            if (ls->unknown < 4 && s_frame <= 1)
                ps3_logf("[pvr] registro raro en lista %d offset %u: cmd=0x%08x",
                         l, (unsigned)off, (unsigned)cmd);
            ls->unknown++;
        }
        off += 32;
    }
}

int pvr_scene_finish(void)
{
    static const int order[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
    static const char *names[3] = { "OP", "PT", "TR" };
    int i;

    s_frame++;
    /* (diagnostico del primer frame apagado para la 1.0) */
    if (s_diag_left > 0)
        ps3_logf("[diag] --- frame %d, modo RSX %d (%s) ---", s_frame, g_rsx_dbg, PS3_RSX_DbgName(g_rsx_dbg));

    PS3_Sys_Overlay();              /* contador de FPS (compat/ps3_system.c) */

    /* Cada escena del motor es un frame en pantalla: esperar el flip
     * anterior y apuntar al framebuffer siguiente (ps3_rsx.c). */
    PS3_RSX_PresentBegin();
    if (PS3_RSX_Ready())
        PS3_RSX_BeginScene(s_bg_rgb);

    memset(&s_batch_rs, 0xff, sizeof(s_batch_rs));   /* forzar el primer cambio */
    s_clipped = 0;
    s_badtris = 0;
    s_batch_n = 0;

    for (i = 0; i < 3; i++) {
        replay_list(order[i]);
        batch_flush();
    }

    /* Estadisticas: el primer frame, y despues cada ~10 segundos. */
    if (s_frame == 1) {
        for (i = 0; i < 3; i++) {
            int l = order[i];
            list_stats_t *ls = &s_stats[l];
            ps3_logf("[pvr] frame %d lista %s: %u bytes, %d registros, %d headers, "
                     "%d tiras, %d triangulos, %d salteadas, %d sprites, %d raros%s",
                     s_frame, names[i], (unsigned)s_list[l].len, ls->records,
                     ls->headers + ls->sprite_hdrs, ls->strips, ls->tris,
                     ls->skipped, ls->sprites, ls->unknown,
                     s_list[l].overflowed ? "  *** LLENA ***" : "");
        }
        ps3_logf("[pvr] triangulos recortados contra la pantalla en este frame: %d, descartados por vertice invalido: %d", s_clipped, s_badtris);
        ps3_logf("[pvr] texturas: %s, %d vivas, %d creadas, %d fallidas, "
                 "VRAM del PVR usada %u KB de 8192",
                 s_textures_on ? "si" : "NO (archivo NOTEX)",
                 s_tex_live, s_tex_created, s_tex_failed,
                 (unsigned)(s_mem_bytes / 1024));
        if (s_fog_valid)
            ps3_logf("[pvr] fog: color %02x%02x%02x, de %.0f a %.0f",
                     s_fog_r & 0xff, s_fog_g & 0xff, s_fog_b & 0xff,
                     s_fog_start, s_fog_end);
    }
    s_diag_left = 0;                /* el diagnostico dura un solo frame */
    PS3_RSX_PresentEnd();           /* flip */
    mem_flush_deferred();           /* ahora si: nada de este frame la lee */
    return 0;
}

#ifdef PS3_HOST_DEBUG
/* Solo para las pruebas en x86: vuelca registros de una lista. */
void ps3_pvr_debug_dump(int l, uint32_t from, uint32_t count)
{
    uint32_t off;
    for (off = from; off < from + count * 32 && off + 32 <= s_list[l].len; off += 32) {
        const uint32_t *w = (const uint32_t *)(s_list[l].buf + off);
        printf("%6u: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)off,
               w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}
#endif

#ifdef PS3_HOST_DEBUG
/* Solo x86: vuelca la memoria de una textura y la paleta. */
void ps3_pvr_debug_texdump(const void *ptr, int w, int h, uint32_t fmt, const char *path)
{
    FILE *f = fopen(path, "wb");
    memrec_t *r = mem_find((uintptr_t)ptr);
    uint32_t hdr[5] = { (uint32_t)w, (uint32_t)h, fmt, r ? r->size : 0, r ? r->linear : 9 };
    fwrite(hdr, 4, 5, f);
    fwrite(s_pal, 4, 1024, f);
    fwrite(ptr, 1, r ? r->size : (uint32_t)(w * h), f);
    fclose(f);
}
#endif
