/* ps3_rsx.c -- backend de dibujo sobre el RSX.
 *
 * Recibe triangulos ya en clip space (los arma compat/ps3_pvr.c a partir del
 * stream del PVR) y los dibuja. Todo lo que es GCM vive aca.
 *
 * Todas las llamadas y constantes de este archivo son las mismas que usa
 * IoQuake3-PS3 (code/gl/), que compila en este toolchain y corre en
 * hardware. Los shaders son los suyos, precompilados (ps3_shader_data.h).
 *
 * VERTICES: un ring en memoria del RSX partido en 3 segmentos, uno por
 * framebuffer. PS3_Video_BeginFrame espera a que haya a lo sumo un flip en
 * vuelo, asi que cuando un segmento se vuelve a usar el RSX ya termino el
 * frame que lo leia (tres frames atras).
 *
 * VIEWPORT: el motor dibuja en 640x480 (4:3). Se centra en la pantalla con
 * barras a los costados, sin estirar.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "ps3_rsx.h"
#include "ps3_video.h"
#include "ps3_log.h"
#include <lv2/systime.h>

int PS3_Sys_FpsUncap(void);    /* compat/ps3_system.c */
#include "ps3_shader_data.h"

#define RING_SEGMENTS   3
#define RING_SEG_SIZE   (3 * 1024 * 1024)

static int      s_ready = 0;

static uint8_t *s_ring = NULL;
static u32      s_ring_off = 0;
static u32      s_seg = 0;
static u32      s_head = 0;
static int      s_warned_full = 0;

/* Shaders */
static rsxVertexProgram   *s_vp = NULL;
static void               *s_vp_ucode = NULL;
static u32                 s_vp_ucode_size = 0;
static rsxProgramConst    *s_mvp_const = NULL;

static rsxFragmentProgram *s_fp_color = NULL;
static void               *s_fp_color_ucode = NULL;
static u32                 s_fp_color_size = 0;
static u32                 s_fp_color_off = 0;

static rsxFragmentProgram *s_fp_tex = NULL;         /* color * textura */
static void               *s_fp_tex_ucode = NULL;
static u32                 s_fp_tex_size = 0;
static u32                 s_fp_tex_off = 0;

static int                 s_cur_fp = -1;          /* 0 color, 1 textura */

/* ------------------------------------------------------------------ */
/* Texturas                                                            */
/* ------------------------------------------------------------------ */

#define TEX_REMAP_IDENTITY  0x00AAE4       /* el mismo que IoQuake3-PS3 */
#define TEX_FREE_DELAY      4              /* frames antes de liberar  */
#define TEX_PENDING_MAX     4096

struct ps3_rtex {
    void       *data;
    u32         offset;
    u16         w, h;
    int         swz;         /* 1 = swizzled (Morton), 0 = lineal */
    gcmTexture  gt;
};

typedef struct {
    ps3_rtex_t *t;
    u32         due;
} pending_free_t;

static pending_free_t s_pending[TEX_PENDING_MAX];
static int            s_npending = 0;
static u32            s_tex_bytes = 0;

/* Estado actual en el RSX, para no reenviar lo mismo. */
static ps3_rstate_t s_cur;
static int          s_cur_valid = 0;

/* Las coordenadas llegan en clip space: la matriz es la identidad. */
static float s_identity[16] __attribute__((aligned(16))) = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
};

static u32 s_frames = 0;
static int s_nodepth = 0;          /* archivo NODEPTH en USRDIR: sin test de profundidad */
/* Texturas swizzled (orden Morton del RSX), como IoQuake3-PS3 en produccion.
 * Las lineales solo andan bien con CLAMP; el motor usa REPEAT con uv
 * negativas y mayores que 1 en todas las paredes y pisos. Archivo LINEAR en
 * USRDIR vuelve a las lineales (para comparar). */
static int s_swizzle = 1;
/* Hay texturas nuevas escritas por la CPU: invalidar la cache de texturas
 * del RSX antes de muestrear (como hace ps3gl de Xash3D). */
static int s_tex_dirty = 0;

/* Offset swizzled de (x,y) en una textura de 2^lw x 2^lh: bits intercalados,
 * primero x. Cuando una dimension se agota, siguen los bits de la otra. */
static u32 swz_offset(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 off = 0, bit = 0;
    while (lw || lh) {
        if (lw) { off |= (x & 1u) << bit; x >>= 1; bit++; lw--; }
        if (lh) { off |= (y & 1u) << bit; y >>= 1; bit++; lh--; }
    }
    return off;
}

static u32 ilog2u(u32 v) { u32 r = 0; while (v > 1) { v >>= 1; r++; } return r; }
static u32 s_draws = 0, s_verts = 0;

/* ------------------------------------------------------------------ */

static int load_shaders(void)
{
    void *ucode;

    s_vp = (rsxVertexProgram *)shader_vp_data;
    rsxVertexProgramGetUCode(s_vp, &s_vp_ucode, &s_vp_ucode_size);
    s_mvp_const = rsxVertexProgramGetConst(s_vp, "mvp");
    ps3_logf("[rsx] vertex program: %u bytes de ucode, mvp=%p",
             (unsigned)s_vp_ucode_size, (void *)s_mvp_const);
    if (!s_vp_ucode || !s_mvp_const) {
        ps3_log("[rsx] *** el vertex program no cargo ***");
        return -1;
    }

    /* El ucode del fragment program tiene que estar en memoria del RSX. */
    s_fp_color = (rsxFragmentProgram *)shader_fp_coloronly_data;
    rsxFragmentProgramGetUCode(s_fp_color, &ucode, &s_fp_color_size);
    s_fp_color_ucode = rsxMemalign(64, s_fp_color_size);
    if (!s_fp_color_ucode) {
        ps3_log("[rsx] *** rsxMemalign fallo para el fragment program ***");
        return -1;
    }
    memcpy(s_fp_color_ucode, ucode, s_fp_color_size);
    rsxAddressToOffset(s_fp_color_ucode, &s_fp_color_off);
    ps3_logf("[rsx] fragment program (color): %u bytes en offset 0x%08x",
             (unsigned)s_fp_color_size, (unsigned)s_fp_color_off);

    s_fp_tex = (rsxFragmentProgram *)shader_fp_modulate_data;
    rsxFragmentProgramGetUCode(s_fp_tex, &ucode, &s_fp_tex_size);
    s_fp_tex_ucode = rsxMemalign(64, s_fp_tex_size);
    if (!s_fp_tex_ucode) {
        ps3_log("[rsx] *** rsxMemalign fallo para el fragment program de textura ***");
        return -1;
    }
    memcpy(s_fp_tex_ucode, ucode, s_fp_tex_size);
    rsxAddressToOffset(s_fp_tex_ucode, &s_fp_tex_off);
    ps3_logf("[rsx] fragment program (textura): %u bytes en offset 0x%08x",
             (unsigned)s_fp_tex_size, (unsigned)s_fp_tex_off);
    return 0;
}

int PS3_RSX_Init(void)
{
    if (s_ready) return 0;
    if (!PS3_Video_Context()) {
        ps3_log("[rsx] *** no hay contexto de video ***");
        return -1;
    }

    if (load_shaders() < 0) return -1;

    s_ring = (uint8_t *)rsxMemalign(128, RING_SEGMENTS * RING_SEG_SIZE);
    if (!s_ring) {
        ps3_logf("[rsx] *** rsxMemalign fallo para el ring de vertices (%u bytes) ***",
                 (unsigned)(RING_SEGMENTS * RING_SEG_SIZE));
        return -1;
    }
    rsxAddressToOffset(s_ring, &s_ring_off);
    ps3_logf("[rsx] ring de vertices: %d x %u KB en offset 0x%08x",
             RING_SEGMENTS, (unsigned)(RING_SEG_SIZE / 1024), (unsigned)s_ring_off);

    s_nodepth = (access(PS3_USRDIR "/NODEPTH", F_OK) == 0);
    if (s_nodepth) ps3_log("[rsx] archivo NODEPTH presente: test de profundidad APAGADO");
    s_swizzle = (access(PS3_USRDIR "/LINEAR", F_OK) != 0);
    ps3_logf("[rsx] texturas %s", s_swizzle ? "SWIZZLED (normal)" : "LINEALES (archivo LINEAR)");

    s_ready = 1;
    ps3_log("[rsx] backend listo");
    return 0;
}

void PS3_RSX_Shutdown(void)
{
    if (!s_ready) return;
    s_ready = 0;
    /* El RSX ya esta parado (PS3_Video_Shutdown hace rsxFinish antes o
     * despues, y liberar memoria que el RSX no lee mas es seguro). */
    if (s_ring)           { rsxFree(s_ring);           s_ring = NULL; }
    if (s_fp_color_ucode) { rsxFree(s_fp_color_ucode); s_fp_color_ucode = NULL; }
    if (s_fp_tex_ucode)   { rsxFree(s_fp_tex_ucode);   s_fp_tex_ucode = NULL; }
}

int PS3_RSX_Ready(void) { return s_ready; }

/* ------------------------------------------------------------------ */

/* Un frame del motor = una escena del PVR. En el Dreamcast el flip lo hace
 * el hardware al terminar el render; aca se encola a mano. En modo 30 fps
 * (Opciones > Video) se espera a que pasen 2 vblanks entre flips. */
void PS3_RSX_PresentBegin(void)
{
    PS3_Video_BeginFrame();
}

void PS3_RSX_PresentEnd(void)
{
    static u64 last_us = 0;
    u64 now;
    PS3_Video_EndFrame();
    if (!PS3_Sys_FpsUncap()) {
        now = (u64)sysGetSystemTime();
        if (last_us && now - last_us < 33000)
            usleep((useconds_t)(33000 - (now - last_us)));
        last_us = (u64)sysGetSystemTime();
    }
}

void PS3_RSX_BeginScene(uint32_t bg_rgb)
{
    gcmContextData *ctx = PS3_Video_Context();
    u32 W = PS3_Video_Width(), H = PS3_Video_Height();
    u32 vw, vh, vx, vy;
    float scale[4], offset[4];

    if (!s_ready || !ctx) return;

    s_seg  = (u32)PS3_Video_FrameIndex() % RING_SEGMENTS;
    s_head = 0;

    /* Pantalla completa en negro (las barras laterales), profundidad al fondo. */
    rsxSetScissor(ctx, 0, 0, W, H);
    rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
                         GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    rsxSetClearColor(ctx, 0xff000000u);
    rsxSetClearDepthStencil(ctx, 0xffffff00u);
    rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B |
                         GCM_CLEAR_A | GCM_CLEAR_S | GCM_CLEAR_Z);

    /* Area de juego 4:3, centrada, con el color de fondo del PVR. */
    vh = H;
    vw = (H * 4) / 3;
    if (vw > W) { vw = W; vh = (W * 3) / 4; }
    vx = (W - vw) / 2;
    vy = (H - vh) / 2;

    rsxSetScissor(ctx, vx, vy, vw, vh);
    rsxSetClearColor(ctx, 0xff000000u | (bg_rgb & 0xffffffu));
    rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

    scale[0]  = vw * 0.5f;   scale[1]  = vh * -0.5f;  scale[2]  = 0.5f; scale[3]  = 0.0f;
    offset[0] = vx + vw * 0.5f;
    offset[1] = vy + vh * 0.5f;
    offset[2] = 0.5f;
    offset[3] = 0.0f;
    rsxSetViewport(ctx, (u16)vx, (u16)vy, (u16)vw, (u16)vh, 0.0f, 1.0f, scale, offset);
    rsxSetViewportClip(ctx, 0, W, H);

    /* Estado base. El PVR no hace culling util para este motor (las tiras
     * vienen con cualquier sentido de giro). */
    rsxSetCullFaceEnable(ctx, GCM_FALSE);
    rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
    rsxSetAlphaTestEnable(ctx, GCM_FALSE);
    rsxSetDepthTestEnable(ctx, s_nodepth ? GCM_FALSE : GCM_TRUE);
    rsxSetBlendEnable(ctx, GCM_FALSE);
    s_cur_valid = 0;

    rsxLoadVertexProgram(ctx, s_vp, s_vp_ucode);
    rsxSetVertexProgramParameter(ctx, s_vp, s_mvp_const, s_identity);
    rsxLoadFragmentProgramLocation(ctx, s_fp_color, s_fp_color_off, GCM_LOCATION_RSX);
    s_cur_fp = 0;
    rsxTextureControl(ctx, 0, GCM_FALSE, 0, 0, 0);

    s_frames++;

    /* Texturas liberadas hace suficientes frames: el RSX ya no las lee. */
    {
        int i = 0;
        while (i < s_npending) {
            if ((int)(s_frames - s_pending[i].due) >= 0) {
                ps3_rtex_t *t = s_pending[i].t;
                s_tex_bytes -= (u32)t->w * t->h * 4;
                rsxFree(t->data);
                free(t);
                s_pending[i] = s_pending[--s_npending];
            } else {
                i++;
            }
        }
    }
    if (s_frames == 1)
        ps3_logf("[rsx] primer frame: viewport %ux%u en (%u,%u), fondo 0x%06x",
                 (unsigned)vw, (unsigned)vh, (unsigned)vx, (unsigned)vy,
                 (unsigned)(bg_rgb & 0xffffff));
}

static u32 blend_factor(int f)
{
    switch (f) {
    case RSX_BLEND_ZERO:          return GCM_ZERO;
    case RSX_BLEND_ONE:           return GCM_ONE;
    case RSX_BLEND_DESTCOLOR:     return GCM_DST_COLOR;
    case RSX_BLEND_INVDESTCOLOR:  return GCM_ONE_MINUS_DST_COLOR;
    case RSX_BLEND_SRCALPHA:      return GCM_SRC_ALPHA;
    case RSX_BLEND_INVSRCALPHA:   return GCM_ONE_MINUS_SRC_ALPHA;
    case RSX_BLEND_DESTALPHA:     return GCM_DST_ALPHA;
    case RSX_BLEND_INVDESTALPHA:  return GCM_ONE_MINUS_DST_ALPHA;
    default:                      return GCM_ONE;
    }
}

static u32 depth_func(int f)
{
    switch (f) {
    case RSX_DEPTH_NEVER:    return GCM_NEVER;
    case RSX_DEPTH_LESS:     return GCM_LESS;
    case RSX_DEPTH_EQUAL:    return GCM_EQUAL;
    case RSX_DEPTH_LEQUAL:   return GCM_LEQUAL;
    case RSX_DEPTH_GREATER:  return GCM_GREATER;
    case RSX_DEPTH_NOTEQUAL: return GCM_NOTEQUAL;
    case RSX_DEPTH_GEQUAL:   return GCM_GEQUAL;
    default:                 return GCM_ALWAYS;
    }
}

/* Modo de diagnostico (se cicla con R3 desde main.c). Pisa el estado que
 * pide el PVR para aislar que parte del pipeline mata la vista 3D. */
int g_rsx_dbg = 0;
int g_rsx_drawlog = 0;   /* draws que quedan por loguear (frame de diagnostico) */
const char *PS3_RSX_DbgName(int m)
{
    switch (m) {
    case 0: return "normal";
    case 1: return "sin alpha test";
    case 2: return "sin blending";
    case 3: return "sin depth test (ALWAYS, sin escritura)";
    case 4: return "sin alpha test + sin blending + sin depth";
    case 5: return "sin texturas (fp de color, en vivo)";
    case 6: return "texturas con CLAMP y NEAREST";
    case 7: return "sin cielo";
    case 8: return "sin sprites";
    case 9: return "sin triangulos que se salen de pantalla";
    case 10: return "sin cielo y sin sprites";
    case 11: return "recorte con el margen viejo (+32 px fuera de pantalla)";
    case 12: return "triangulos recortados SIN textura";
    case 13: return "SOLO los triangulos recortados";
    default: return "?";
    }
}
static void dbg_modify(ps3_rstate_t *e)
{
    switch (g_rsx_dbg) {
    case 1: e->alpha_test = 0; break;
    case 2: e->blend = 0; break;
    case 3: e->depth_func = RSX_DEPTH_ALWAYS; e->depth_write = 0; break;
    case 4: e->alpha_test = 0; e->blend = 0;
            e->depth_func = RSX_DEPTH_ALWAYS; e->depth_write = 0; break;
    case 5: e->tex = NULL; e->alpha_test = 0; break;
    case 6: e->clamp = 3; e->filter = 0; break;
    default: break;
    }
}

static void apply_state(gcmContextData *ctx, const ps3_rstate_t *st)
{
    ps3_rstate_t dbg_tmp;
    if (g_rsx_dbg) { dbg_tmp = *st; dbg_modify(&dbg_tmp); st = &dbg_tmp; }

    if (s_cur_valid && memcmp(st, &s_cur, sizeof(*st)) == 0)
        return;

    if (!s_cur_valid || st->blend != s_cur.blend ||
        st->src != s_cur.src || st->dst != s_cur.dst) {
        rsxSetBlendEnable(ctx, st->blend ? GCM_TRUE : GCM_FALSE);
        if (st->blend) {
            u32 sf = blend_factor(st->src), df = blend_factor(st->dst);
            rsxSetBlendFunc(ctx, sf, df, sf, df);
            rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
        }
    }

    if (!s_cur_valid || st->depth_func != s_cur.depth_func)
        rsxSetDepthFunc(ctx, depth_func(st->depth_func));

    if (!s_cur_valid || st->depth_write != s_cur.depth_write)
        rsxSetDepthWriteEnable(ctx, st->depth_write ? GCM_TRUE : GCM_FALSE);

    if (!s_cur_valid || st->alpha_test != s_cur.alpha_test) {
        rsxSetAlphaTestEnable(ctx, st->alpha_test ? GCM_TRUE : GCM_FALSE);
        if (st->alpha_test)
            rsxSetAlphaFunc(ctx, GCM_GREATER, 0);
    }

    /* Textura y fragment program */
    {
        int want_fp = st->tex ? 1 : 0;
        if (want_fp != s_cur_fp) {
            if (want_fp)
                rsxLoadFragmentProgramLocation(ctx, s_fp_tex, s_fp_tex_off, GCM_LOCATION_RSX);
            else
                rsxLoadFragmentProgramLocation(ctx, s_fp_color, s_fp_color_off, GCM_LOCATION_RSX);
            s_cur_fp = want_fp;
        }
        if (st->tex && (!s_cur_valid || st->tex != s_cur.tex ||
                        st->filter != s_cur.filter || st->clamp != s_cur.clamp)) {
            u32 f = st->filter ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
            u32 wu = (st->clamp & 1) ? GCM_TEXTURE_CLAMP_TO_EDGE :
                     (st->clamp & 4) ? GCM_TEXTURE_MIRRORED_REPEAT : GCM_TEXTURE_REPEAT;
            u32 wv = (st->clamp & 2) ? GCM_TEXTURE_CLAMP_TO_EDGE :
                     (st->clamp & 8) ? GCM_TEXTURE_MIRRORED_REPEAT : GCM_TEXTURE_REPEAT;
            if (s_tex_dirty) {
                __asm__ volatile("sync" ::: "memory");
                rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
                s_tex_dirty = 0;
            }
            rsxLoadTexture(ctx, 0, &st->tex->gt);
            rsxTextureControl(ctx, 0, GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
            rsxTextureFilter(ctx, 0, 0, f, f, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
            rsxTextureWrapMode(ctx, 0, wu, wv, GCM_TEXTURE_CLAMP_TO_EDGE,
                               GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                               GCM_TEXTURE_ZFUNC_NEVER, 0);
        }
    }

    s_cur = *st;
    s_cur_valid = 1;
}

void PS3_RSX_DrawTris(const ps3_rstate_t *st, const ps3_rvert_t *v, int n)
{
    gcmContextData *ctx = PS3_Video_Context();
    u32 bytes, off;
    uint8_t *dst;

    if (!s_ready || !ctx || n < 3) return;

    bytes = (u32)n * (u32)sizeof(ps3_rvert_t);
    if (s_head + bytes > RING_SEG_SIZE) {
        /* No se pisa lo que el RSX todavia no leyo: se descarta el lote. */
        if (!s_warned_full) {
            ps3_logf("[rsx] *** segmento de vertices lleno (%u + %u > %u): "
                     "se descartan triangulos ***",
                     (unsigned)s_head, (unsigned)bytes, (unsigned)RING_SEG_SIZE);
            s_warned_full = 1;
        }
        return;
    }

    dst = s_ring + s_seg * RING_SEG_SIZE + s_head;
    off = s_ring_off + s_seg * RING_SEG_SIZE + s_head;
    memcpy(dst, v, bytes);
    /* Cada bloque arranca alineado a 16: si no, los offsets de los atributos
     * se corren y el fetch de vertices del RSX cuelga la consola (nota de
     * IoQuake3-PS3). */
    s_head = (s_head + bytes + 15u) & ~15u;

    apply_state(ctx, st);

    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, off + 0,
                             sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, off + 16,
                             sizeof(ps3_rvert_t), 2, GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, off + 24,
                             sizeof(ps3_rvert_t), 2, GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, off + 32,
                             sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8,
                             GCM_LOCATION_RSX);

    rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, (u32)n);

    if (g_rsx_drawlog > 0) {
        g_rsx_drawlog--;
        ps3_logf("[draw] n=%d tex=%p off=%08x %dx%d swz=%d blend=%d(%d,%d) atest=%d depth=%d wr=%d clamp=%d filt=%d ring=%08x",
                 n, (void *)st->tex, st->tex ? (unsigned)st->tex->offset : 0u,
                 st->tex ? st->tex->w : 0, st->tex ? st->tex->h : 0, st->tex ? st->tex->swz : 0,
                 st->blend, st->src, st->dst, st->alpha_test, st->depth_func, st->depth_write,
                 st->clamp, st->filter, (unsigned)off);
    }
    s_draws++;
    s_verts += (u32)n;
    if (s_frames == 1 && s_draws == 1)
        ps3_logf("[rsx] primer draw: %d vertices", n);
}

/* ------------------------------------------------------------------ */

ps3_rtex_t *PS3_RSX_TexCreate(int w, int h, const uint32_t *argb)
{
    ps3_rtex_t *t;
    u32 bytes;

    if (!s_ready || w <= 0 || h <= 0) return NULL;

    t = (ps3_rtex_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;

    bytes = (u32)w * (u32)h * 4;
    t->data = rsxMemalign(128, bytes);
    if (!t->data) {
        static int warned = 0;
        if (!warned) {
            ps3_logf("[rsx] *** rsxMemalign fallo para una textura de %dx%d "
                     "(%u KB de texturas en uso) ***", w, h, (unsigned)(s_tex_bytes / 1024));
            warned = 1;
        }
        free(t);
        return NULL;
    }
    /* 0xAARRGGBB nativo (big-endian) = bytes A,R,G,B = A8R8G8B8 del RSX. */
    if (s_swizzle && (w & (w - 1)) == 0 && (h & (h - 1)) == 0) {
        u32 *dst = (u32 *)t->data;
        u32 lw = ilog2u((u32)w), lh = ilog2u((u32)h), x, y;
        for (y = 0; y < (u32)h; y++)
            for (x = 0; x < (u32)w; x++)
                dst[swz_offset(x, y, lw, lh)] = argb[y * (u32)w + x];
        t->swz = 1;
    } else {
        memcpy(t->data, argb, bytes);
        t->swz = 0;
    }
    rsxAddressToOffset(t->data, &t->offset);
    s_tex_dirty = 1;
    t->w = (u16)w;
    t->h = (u16)h;

    memset(&t->gt, 0, sizeof(t->gt));
    t->gt.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 |
                      (t->swz ? GCM_TEXTURE_FORMAT_SWZ : GCM_TEXTURE_FORMAT_LIN);
    t->gt.mipmap    = 1;
    t->gt.dimension = GCM_TEXTURE_DIMS_2D;
    t->gt.cubemap   = GCM_FALSE;
    t->gt.remap     = TEX_REMAP_IDENTITY;
    t->gt.width     = (u16)w;
    t->gt.height    = (u16)h;
    t->gt.depth     = 1;
    t->gt.location  = GCM_LOCATION_RSX;
    t->gt.pitch     = t->swz ? 0 : (u32)w * 4;
    t->gt.offset    = t->offset;

    s_tex_bytes += bytes;
    return t;
}

void PS3_RSX_TexRelease(ps3_rtex_t *t)
{
    if (!t) return;
    /* Puede estar referenciada por frames que el RSX todavia no dibujo:
     * se libera recien TEX_FREE_DELAY frames despues. */
    if (s_npending < TEX_PENDING_MAX) {
        s_pending[s_npending].t   = t;
        s_pending[s_npending].due = s_frames + TEX_FREE_DELAY;
        s_npending++;
    }
    /* Si la cola se llenara, se pierde memoria antes que arriesgar un cuelgue. */
}
