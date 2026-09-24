/* ps3_video.h -- init de RSX/GCM, framebuffers y flip. */

#ifndef PS3_VIDEO_H
#define PS3_VIDEO_H

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

int  PS3_Video_Init(void);      /* 0 = ok, <0 = error (ver log) */
void PS3_Video_Shutdown(void);

void PS3_Video_BeginFrame(void);
void PS3_Video_EndFrame(void);

/* Pinta el framebuffer entero. color = 0xAARRGGBB */
void PS3_Video_Clear(u32 color);

u32  PS3_Video_Width(void);
u32  PS3_Video_Height(void);
gcmContextData *PS3_Video_Context(void);

/* Indice del framebuffer que se esta dibujando (0..2). ps3_rsx.c lo usa para
 * elegir el segmento del ring de vertices que ya no esta en vuelo. */
int  PS3_Video_FrameIndex(void);
void PS3_Video_Capture565(unsigned short *dst);   /* 320x240, filas de 512 */

/* Pantalla de texto por CPU (antes de que arranque el motor): titulo,
 * lineas y, si percent >= 0, una barra de progreso. Hace el flip. */
void PS3_Video_TextScreen(const char *title, const char *const *lines, int nlines, int percent);

#endif /* PS3_VIDEO_H */
