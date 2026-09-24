/* compat/dc/pvr.h -- shim del PowerVR de KallistiOS para PS3.
 *
 * En PS3 no hay PVR. Este header existe para que el motor COMPILE sin tocar
 * sus fuentes; las funciones son stubs (ps3_pvr_stub.c) hasta que entre el
 * backend RSX.
 *
 * ESTE ARCHIVO ES LA ESPECIFICACION DEL BACKEND QUE HAY QUE ESCRIBIR.
 * La superficie completa que el motor usa del PVR son 28 funciones y 23
 * constantes -- todo lo que esta aca abajo. El modelo del PVR (compilar un
 * contexto de estado -> header de poligono -> emitir vertices a una lista)
 * mapea bastante directo a GCM: el contexto es el estado del pipeline, el
 * header es el cambio de estado, y la lista es el orden de emision.
 *
 * Diferencias que el backend RSX va a tener que resolver:
 *  - El PVR es tile-based deferred y ORDENA las transparencias solo. El RSX
 *    no: hay que emitir en orden de BSP y usar z-buffer.
 *  - Las texturas paletizadas (PVR_TXRFMT_PAL8BPP) no existen en RSX; hay que
 *    expandir a ARGB al subir (con 256 MB de VRAM no es problema).
 *  - La tabla de fog por hardware no existe; va en el fragment program.
 */

#ifndef PS3_COMPAT_DC_PVR_H
#define PS3_COMPAT_DC_PVR_H

#include <stdint.h>
#include "dc/vector.h"

typedef void *pvr_ptr_t;

/* ---------------- listas ---------------- */
#define PVR_LIST_OP_POLY      0   /* opacos           */
#define PVR_LIST_OP_MOD       1
#define PVR_LIST_TR_POLY      2   /* translucidos     */
#define PVR_LIST_TR_MOD       3
#define PVR_LIST_PT_POLY      4   /* punch-through    */

/* ---------------- comandos ---------------- */
#define PVR_CMD_POLYHDR       0x80840000
#define PVR_CMD_VERTEX        0xe0000000
#define PVR_CMD_VERTEX_EOL    0xf0000000
#define PVR_CMD_USERCLIP      0x20000000

/* ---------------- blending ---------------- */
#define PVR_BLEND_ZERO        0
#define PVR_BLEND_ONE         1
#define PVR_BLEND_DESTCOLOR   2
#define PVR_BLEND_INVDESTCOLOR 3
#define PVR_BLEND_SRCALPHA    4
#define PVR_BLEND_INVSRCALPHA 5
#define PVR_BLEND_DESTALPHA   6
#define PVR_BLEND_INVDESTALPHA 7

/* ---------------- texturas ---------------- */
#define PVR_TXRFMT_NONE       0
#define PVR_TXRFMT_ARGB1555   (0 << 27)
#define PVR_TXRFMT_RGB565     (1 << 27)
#define PVR_TXRFMT_ARGB4444   (2 << 27)
#define PVR_TXRFMT_PAL4BPP    (5 << 27)
#define PVR_TXRFMT_PAL8BPP    (6 << 27)
#define PVR_TXRFMT_BUMP       (4 << 27)
#define PVR_TXRFMT_TWIDDLED   (0 << 26)
#define PVR_TXRFMT_NONTWIDDLED (1 << 26)
#define PVR_TXRFMT_VQ_ENABLE  (1 << 30)
#define PVR_TXRFMT_8BPP_PAL(x)  ((6 << 27) | ((x) << 25))

/* Empaqueta dos coordenadas UV float en un uint32 (dos half-floats). */
#define PVR_PACK_16BIT_UV(u, v)  ps3_pvr_pack_uv((u), (v))
uint32_t ps3_pvr_pack_uv(float u, float v);

#define PVR_TXRLOAD_8BPP      (1 << 0)
#define PVR_TXRLOAD_16BPP     (1 << 1)
#define PVR_TXRLOAD_32BPP     (1 << 2)

#define PVR_FILTER_NONE       0
#define PVR_FILTER_BILINEAR   2
#define PVR_FILTER_TRILINEAR1 3
#define PVR_FILTER_TRILINEAR2 4

#define PVR_UVFLIP_NONE       0
#define PVR_UVFLIP_V          1
#define PVR_UVFLIP_U          2
#define PVR_UVFLIP_UV         3

#define PVR_CLAMP_NONE        0
#define PVR_CLAMP_U           1
#define PVR_CLAMP_V           2
#define PVR_CLAMP_UV          3

#define PVR_TXRENV_REPLACE    0
#define PVR_TXRENV_MODULATE   1
#define PVR_TXRENV_DECAL      2
#define PVR_TXRENV_MODULATEALPHA 3

#define PVR_MIPMAP_DISABLE    0
#define PVR_MIPMAP_ENABLE     1

/* ---------------- estado general ---------------- */
#define PVR_SHADE_FLAT        0
#define PVR_SHADE_GOURAUD     1

#define PVR_CULLING_NONE      0
#define PVR_CULLING_SMALL     1
#define PVR_CULLING_CCW       2
#define PVR_CULLING_CW        3

#define PVR_DEPTHCMP_NEVER    0
#define PVR_DEPTHCMP_LESS     1
#define PVR_DEPTHCMP_EQUAL    2
#define PVR_DEPTHCMP_LEQUAL   3
#define PVR_DEPTHCMP_GREATER  4
#define PVR_DEPTHCMP_GEQUAL   6
#define PVR_DEPTHCMP_ALWAYS   7

#define PVR_DEPTHWRITE_ENABLE  0
#define PVR_DEPTHWRITE_DISABLE 1

#define PVR_ALPHA_DISABLE     0
#define PVR_ALPHA_ENABLE      1

#define PVR_SPECULAR_DISABLE  0
#define PVR_SPECULAR_ENABLE   1

#define PVR_FOG_TABLE         0
#define PVR_FOG_VERTEX        1
#define PVR_FOG_DISABLE       2
#define PVR_FOG_TABLE2        3

#define PVR_CLRCLAMP_DISABLE  0
#define PVR_CLRCLAMP_ENABLE   1

#define PVR_USERCLIP_DISABLE  0
#define PVR_USERCLIP_INSIDE   2
#define PVR_USERCLIP_OUTSIDE  3

#define PVR_MODIFIER_DISABLE  0
#define PVR_MODIFIER_ENABLE   1

#define PVR_PAL_ARGB1555      0
#define PVR_PAL_RGB565        1
#define PVR_PAL_ARGB4444      2
#define PVR_PAL_ARGB8888      3

#define PVR_TA_INPUT          0
#define PVR_BINSIZE_0         0
#define PVR_BINSIZE_8         8
#define PVR_BINSIZE_16        16
#define PVR_BINSIZE_32        32

/* ---------------- structs ---------------- */

/* 32 bytes: el vertice que consume el TA por store queue. */
typedef struct {
    uint32_t flags;
    float    x, y, z;
    float    u, v;
    uint32_t argb;
    uint32_t oargb;
} pvr_vertex_t;

/* 32 bytes: header de estado que precede a cada tira. */
typedef struct {
    uint32_t cmd;
    uint32_t mode1, mode2, mode3;
    uint32_t d1, d2, d3, d4;
} pvr_poly_hdr_t;

typedef struct {
    uint32_t cmd;
    uint32_t mode1, mode2, mode3;
    uint32_t argb;
    uint32_t oargb;
    uint32_t d1, d2;
} pvr_sprite_hdr_t;

/* Sprite con textura: cuatro esquinas, la cuarta se deduce. */
typedef struct {
    uint32_t flags;
    float    ax, ay, az;
    float    bx, by, bz;
    float    cx, cy, cz;
    float    dx, dy;
    uint32_t dummy;
    uint32_t auv, buv, cuv;
} pvr_sprite_txr_t;

/* Contexto: el estado del pipeline antes de compilarlo a un header.
 * Es el equivalente directo del estado de GCM. */
typedef struct {
    int list_type;
    struct {
        int alpha, shading, fog_type, culling, color_clamp, clip_mode,
            modifier_mode, specular, alpha2, fog_type2, color_clamp2;
    } gen;
    struct {
        int src, dst, src_enable, dst_enable,
            src2, dst2, src_enable2, dst_enable2;
    } blend;
    struct {
        int       enable, filter, mipmap, mipmap_bias, uv_flip, uv_clamp,
                  alpha, env, width, height, format;
        pvr_ptr_t base;
    } txr, txr2;
    struct {
        int comparison, write;
    } depth;
} pvr_poly_cxt_t;

typedef pvr_poly_cxt_t pvr_sprite_cxt_t;

/* En KOS esto es un uint32_t y pvr_dr_target una MACRO -- por eso el motor
 * lo pasa por valor. Se respeta esa forma para no tocar los call sites. */
typedef uint32_t pvr_dr_state_t;

pvr_vertex_t *ps3_pvr_dr_target(void);
#define pvr_dr_target(state)   ps3_pvr_dr_target()

typedef struct {
    int      opb_sizes[5];
    int      vertex_buf_size;
    int      dma_enabled;
    int      fsaa_enabled;
    int      autosort_disabled;
} pvr_init_params_t;

/* ---------------- API (28 funciones) ---------------- */
int   pvr_init(pvr_init_params_t *params);
int   pvr_shutdown(void);

void  pvr_set_bg_color(float r, float g, float b);
void  pvr_set_zclip(float zc);

int   pvr_scene_begin(void);
int   pvr_scene_finish(void);
int   pvr_list_begin(int list);
int   pvr_list_finish(void);
int   pvr_list_prim(int list, void *data, int size);

void  pvr_poly_cxt_col(pvr_poly_cxt_t *dst, int list);
void  pvr_poly_cxt_txr(pvr_poly_cxt_t *dst, int list, int textureformat,
                       int tw, int th, pvr_ptr_t textureaddr, int filtering);
void  pvr_poly_compile(pvr_poly_hdr_t *dst, pvr_poly_cxt_t *src);

void  pvr_sprite_cxt_col(pvr_sprite_cxt_t *dst, int list);
void  pvr_sprite_cxt_txr(pvr_sprite_cxt_t *dst, int list, int textureformat,
                         int tw, int th, pvr_ptr_t textureaddr, int filtering);
void  pvr_sprite_compile(pvr_sprite_hdr_t *dst, pvr_sprite_cxt_t *src);

pvr_ptr_t pvr_mem_malloc(size_t size);
void      pvr_mem_free(pvr_ptr_t chunk);
uint32_t  pvr_mem_available(void);

void  pvr_txr_load(void *src, pvr_ptr_t dst, uint32_t count);
void  pvr_txr_load_ex(void *src, pvr_ptr_t dst, uint32_t w, uint32_t h, uint32_t flags);

void  pvr_set_pal_format(int fmt);
void  pvr_set_pal_entry(uint32_t idx, uint32_t value);

void  pvr_fog_table_color(float a, float r, float g, float b);
void  pvr_fog_table_linear(float start, float end);
void  pvr_fog_table_custom(float tbl1[]);
uint32_t pvr_pack_bump(float h, float t, float q);

void  pvr_dr_init(pvr_dr_state_t *vtx_buf_ptr);
void  pvr_dr_commit(void *addr);

/* Store queues del SH4: en PS3 no existen, es un memcpy. */
#define SQ_MASK_DEST(d)   ((void *)(d))
void  sq_fast_cpy(void *dest, const void *src, size_t n);
void  sq_cpy(void *dest, const void *src, size_t n);
void  sq_set32(void *dest, uint32_t c, size_t n);

void  pvr_set_vertbuf(int list, void *buffer, int len);
void *pvr_vertbuf_tail(int list);
void  pvr_vertbuf_written(int list, uint32_t amt);

#endif /* PS3_COMPAT_DC_PVR_H */
