/* ps3_video.c -- RSX/GCM: init, framebuffers, surface y flip.
 *
 * Patron tomado de IoQuake3-PS3 (code/sys/ps3_glimp.c), que es codigo probado
 * en hardware. Todavia NO hay shaders ni geometria: esta etapa solo valida que
 * el RSX arranca, que el surface queda bien configurado y que el flip corre.
 *
 * Trampas ya contempladas (ver resumen de traspaso):
 *  - Init idempotente: rsxInit dos veces filtra el contexto anterior.
 *  - Shutdown: gcmSetFlipHandler(NULL) PRIMERO, despues rsxFree.
 *  - Compilar este archivo a -O1 -fno-inline (ver Makefile): los syscall stubs
 *    de PSL1GHT son simbolos de indireccion de datos, no funciones normales, y
 *    el inlining agresivo rompe su resolucion en link.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>

#include "ps3_video.h"
#include "ps3_log.h"
#include "ps3_font.h"

/* Triple buffer: con 3 buffers siempre hay uno libre para la CPU. */
#define RSX_FB_COUNT    3
#define RSX_FB_ALIGN    64
#define RSX_CB_SIZE     (1 * 1024 * 1024)    /* command buffer */
#define RSX_HOST_SIZE   (32 * 1024 * 1024)   /* IO buffer */

static gcmContextData *s_ctx = NULL;
static int   s_ready = 0;               /* guard de idempotencia */

static u32   s_width  = 1280;
static u32   s_height = 720;
static u32   s_pitch  = 0;

static u32  *s_color_buffer[RSX_FB_COUNT];
static u32   s_color_offset[RSX_FB_COUNT];
static u32  *s_depth_buffer = NULL;
static u32   s_depth_offset = 0;

static int   s_cur_fb = 0;
static int   s_cur_rt = -1;

static volatile u32 s_flip_queued    = 0;
static volatile u32 s_flip_completed = 0;

static void flip_handler(const u32 head)
{
    (void)head;
    s_flip_completed++;
}

static void set_render_target(int index)
{
    gcmSurface sf;
    int i;

    if (index == s_cur_rt) return;
    s_cur_rt = index;

    memset(&sf, 0, sizeof(sf));

    sf.colorFormat      = GCM_SURFACE_A8R8G8B8;
    sf.colorTarget      = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0]   = s_color_offset[index];
    sf.colorPitch[0]    = s_pitch;

    for (i = 1; i < 4; i++) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorOffset[i]   = s_color_offset[index];
        sf.colorPitch[i]    = 64;
    }

    sf.depthFormat   = GCM_SURFACE_ZETA_Z24S8;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset   = s_depth_offset;
    sf.depthPitch    = s_width * 4;

    sf.type      = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;

    sf.width  = s_width;
    sf.height = s_height;
    sf.x = 0;
    sf.y = 0;

    rsxSetSurface(s_ctx, &sf);
}

static int alloc_framebuffers(void)
{
    u32 color_size, depth_size;
    int i;

    s_pitch    = s_width * 4;                  /* ARGB8888 */
    color_size = s_pitch * s_height;
    depth_size = s_width * s_height * 4;       /* Z24S8 */

    for (i = 0; i < RSX_FB_COUNT; i++) {
        s_color_buffer[i] = (u32 *)rsxMemalign(RSX_FB_ALIGN, color_size);
        if (!s_color_buffer[i]) {
            ps3_logf("[video] FATAL rsxMemalign fallo en color buffer %d (%u bytes)",
                     i, (unsigned)color_size);
            return -1;
        }
        rsxAddressToOffset(s_color_buffer[i], &s_color_offset[i]);
        gcmSetDisplayBuffer(i, s_color_offset[i], s_pitch, s_width, s_height);
    }

    s_depth_buffer = (u32 *)rsxMemalign(RSX_FB_ALIGN, depth_size);
    if (!s_depth_buffer) {
        ps3_log("[video] FATAL rsxMemalign fallo en depth buffer");
        return -1;
    }
    rsxAddressToOffset(s_depth_buffer, &s_depth_offset);

    ps3_logf("[video] framebuffers ok: %ux%u pitch=%u color=%u KB depth=%u KB",
             (unsigned)s_width, (unsigned)s_height, (unsigned)s_pitch,
             (unsigned)(color_size / 1024), (unsigned)(depth_size / 1024));
    return 0;
}

int PS3_Video_Init(void)
{
    void *host_addr;
    s32   ret;
    s32   vid_res    = VIDEO_RESOLUTION_720;
    u8    vid_aspect = VIDEO_ASPECT_16_9;
    videoResolution res;
    videoConfiguration vconfig;

    /* Idempotencia: rsxInit dos veces filtra el contexto anterior. */
    if (s_ready) {
        ps3_log("[video] PS3_Video_Init: ya inicializado, early-return");
        return 0;
    }

    ps3_log("[video] PS3_Video_Init: entrando");

    host_addr = memalign(1024 * 1024, RSX_HOST_SIZE);
    if (!host_addr) {
        ps3_log("[video] FATAL memalign fallo para el IO buffer de 32MB");
        return -1;
    }
    ps3_logf("[video] host_addr=%p cb=0x%x io=0x%x",
             host_addr, RSX_CB_SIZE, RSX_HOST_SIZE);

    ret = rsxInit(&s_ctx, RSX_CB_SIZE, RSX_HOST_SIZE, host_addr);
    ps3_logf("[video] rsxInit ret=%d ctx=%p", (int)ret, (void *)s_ctx);
    if (ret != 0 || !s_ctx) {
        ps3_log("[video] FATAL rsxInit fallo");
        return -1;
    }

    /* 720p si la TV lo soporta; si no, el modo por defecto del televisor. */
    if (!videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720,
                                        VIDEO_ASPECT_16_9, 0)) {
        videoState state;
        videoGetState(0, 0, &state);
        vid_res    = state.displayMode.resolution;
        vid_aspect = state.displayMode.aspect;
        ps3_log("[video] 720p no disponible, usando el modo por defecto de la TV");
    }

    videoGetResolution(vid_res, &res);
    s_width  = res.width;
    s_height = res.height;
    ps3_logf("[video] resolucion: %ux%u", (unsigned)s_width, (unsigned)s_height);

    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = vid_res;
    vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch      = s_width * 4;
    vconfig.aspect     = vid_aspect;
    videoConfigure(0, &vconfig, NULL, 0);
    ps3_log("[video] videoConfigure ok");

    if (alloc_framebuffers() < 0) return -1;

    s_cur_fb         = 0;
    s_cur_rt         = -1;
    s_flip_queued    = 0;
    s_flip_completed = 0;

    gcmSetFlipHandler(flip_handler);
    set_render_target(s_cur_fb);

    s_ready = 1;
    ps3_log("[video] PS3_Video_Init: OK");
    return 0;
}

void PS3_Video_Shutdown(void)
{
    int i;

    if (!s_ready) return;
    ps3_log("[video] shutdown");

    /* ORDEN OBLIGATORIO: primero desenganchar el flip handler. */
    gcmSetFlipHandler(NULL);

    rsxFinish(s_ctx, 1);

    for (i = 0; i < RSX_FB_COUNT; i++) {
        if (s_color_buffer[i]) {
            rsxFree(s_color_buffer[i]);
            s_color_buffer[i] = NULL;
        }
    }
    if (s_depth_buffer) {
        rsxFree(s_depth_buffer);
        s_depth_buffer = NULL;
    }

    s_cur_rt = -1;
    s_ready  = 0;
}

/* Bloquea solo si los dos buffers no mostrados estan en vuelo. */
static void wait_flips(void)
{
    int waited = 0;
    while ((int)(s_flip_queued - s_flip_completed) > RSX_FB_COUNT - 2) {
        usleep(100);
        if (++waited > 20000) {
            ps3_log("[video] WARNING: flip fence timeout, force-sync");
            s_flip_completed = s_flip_queued;
            break;
        }
    }
}

void PS3_Video_BeginFrame(void)
{
    if (!s_ready) return;
    wait_flips();
    set_render_target(s_cur_fb);
}

void PS3_Video_Clear(u32 color)
{
    if (!s_ready) return;

    /* El scissor puede venir en un estado indefinido: fijarlo a pantalla completa. */
    rsxSetScissor(s_ctx, 0, 0, s_width, s_height);

    rsxSetColorMask(s_ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
                           GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    rsxSetClearColor(s_ctx, color);
    rsxSetClearDepthStencil(s_ctx, 0xffffff00);
    rsxClearSurface(s_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B |
                           GCM_CLEAR_A | GCM_CLEAR_S | GCM_CLEAR_Z);
}

void PS3_Video_EndFrame(void)
{
    if (!s_ready) return;

    gcmSetWaitFlip(s_ctx);
    gcmSetFlip(s_ctx, s_cur_fb);
    rsxFlushBuffer(s_ctx);

    s_flip_queued++;
    s_cur_fb = (s_cur_fb + 1) % RSX_FB_COUNT;
}

u32 PS3_Video_Width(void)  { return s_width;  }
u32 PS3_Video_Height(void) { return s_height; }
gcmContextData *PS3_Video_Context(void) { return s_ctx; }
int PS3_Video_FrameIndex(void) { return s_cur_fb; }

/* Captura del ultimo frame presentado, para las transiciones del motor
 * (melt/fade): 320x240 en RGB565, filas de 512 (la textura FB del port de
 * Dreamcast). Solo el area de juego 4:3; muestreo por punto. */
void PS3_Video_Capture565(unsigned short *dst)
{
    u32 vw, vh, vx, vy, x, y;
    const u32 *src;
    static int logged = 0;

    if (!s_ready || !dst) return;
    rsxFinish(s_ctx, 2);                 /* que el RSX termine de dibujar */

    vh = s_height;
    vw = (s_height * 4) / 3;
    if (vw > s_width) { vw = s_width; vh = (s_width * 3) / 4; }
    vx = (s_width - vw) / 2;
    vy = (s_height - vh) / 2;

    src = s_color_buffer[(s_cur_fb + RSX_FB_COUNT - 1) % RSX_FB_COUNT];
    for (y = 0; y < 240; y++) {
        const u32 *row = src + (vy + (y * vh) / 240) * (s_pitch / 4);
        for (x = 0; x < 320; x++) {
            u32 c = row[vx + (x * vw) / 320];
            dst[y * 512 + x] = (unsigned short)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
        }
    }
    if (!logged) { ps3_logf("[video] captura de framebuffer para transicion (%ux%u en %u,%u)", vw, vh, vx, vy); logged = 1; }
}

/* ------------------------------------------------------------------ */
/* Pantalla de texto dibujada por CPU, directo en el framebuffer.        */
/*                                                                      */
/* Sirve antes de que arranque el motor (no hay fuentes ni texturas del */
/* juego todavia): la conversion de la ROM en el primer arranque, y los */
/* errores de datos faltantes. Fuente 8x16 escalada x2 (ps3_font.h).    */
/* ------------------------------------------------------------------ */
#define TXT_SCALE 2

static void txt_rect(u32 *fb, int x, int y, int w, int h, u32 c)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)s_width)  w = (int)s_width - x;
    if (y + h > (int)s_height) h = (int)s_height - y;
    for (j = 0; j < h; j++) {
        u32 *row = fb + (y + j) * (s_pitch / 4) + x;
        for (i = 0; i < w; i++) row[i] = c;
    }
}

static void txt_string(u32 *fb, int x, int y, const char *str, u32 c)
{
    for (; *str; str++, x += 8 * TXT_SCALE) {
        unsigned ch = (unsigned char)*str;
        int gy, gx;
        if (ch < 32 || ch > 126) ch = '?';
        for (gy = 0; gy < 16; gy++) {
            unsigned bits = ps3_font8x16[ch - 32][gy];
            if (!bits) continue;
            for (gx = 0; gx < 8; gx++)
                if (bits & (0x80u >> gx))
                    txt_rect(fb, x + gx * TXT_SCALE, y + gy * TXT_SCALE, TXT_SCALE, TXT_SCALE, c);
        }
    }
}

void PS3_Video_TextScreen(const char *title, const char *const *lines, int nlines, int percent)
{
    u32 *fb;
    int i, y, bw, bx;

    if (!s_ready) return;
    wait_flips();
    rsxFinish(s_ctx, 3);                 /* que el RSX no toque el buffer */

    fb = s_color_buffer[s_cur_fb];
    txt_rect(fb, 0, 0, (int)s_width, (int)s_height, 0xff000000);

    y = (int)s_height / 5;
    if (title) {
        int tw = (int)strlen(title) * 8 * TXT_SCALE;
        txt_string(fb, ((int)s_width - tw) / 2, y, title, 0xffd02020);
        y += 16 * TXT_SCALE * 2;
    }
    for (i = 0; i < nlines; i++) {
        if (lines[i])
            txt_string(fb, (int)s_width / 8, y, lines[i], 0xffe0e0e0);
        y += 16 * TXT_SCALE + 8;
    }

    if (percent >= 0) {
        if (percent > 100) percent = 100;
        bw = (int)s_width * 3 / 4;
        bx = ((int)s_width - bw) / 2;
        y += 24;
        txt_rect(fb, bx - 4, y - 4, bw + 8, 40, 0xff606060);
        txt_rect(fb, bx, y, bw, 32, 0xff101010);
        txt_rect(fb, bx, y, bw * percent / 100, 32, 0xffc03020);
    }

    __asm__ volatile("sync" ::: "memory");
    gcmSetWaitFlip(s_ctx);
    gcmSetFlip(s_ctx, s_cur_fb);
    rsxFlushBuffer(s_ctx);
    s_flip_queued++;
    s_cur_fb = (s_cur_fb + 1) % RSX_FB_COUNT;
}
