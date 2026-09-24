/* ps3_rsx.h -- backend de dibujo sobre el RSX.
 *
 * Interfaz en C pelado, sin headers de PSL1GHT, a proposito: el emulador del
 * TA (compat/ps3_pvr.c) la usa, y asi se puede compilar y linkear tambien en
 * x86 (con un backend falso) para probar la parte que no toca el hardware.
 *
 * El emulador le entrega TRIANGULOS SUELTOS ya en clip space, agrupados por
 * estado. Toda la logica del PVR (tiras, headers, listas, fog, colores) queda
 * del lado del emulador; este backend solo sabe de GCM.
 */

#ifndef PS3_RSX_H
#define PS3_RSX_H

#include <stdint.h>

/* 36 bytes, mismo layout que el vertice de IoQuake3-PS3 (el vertex program
 * precompilado espera exactamente esto):
 *   ATTR0 pos float4 | ATTR8 tc0 float2 | ATTR9 tc1 float2 | ATTR3 color u8x4 */
#pragma pack(push, 1)
typedef struct {
    float    x, y, z, w;      /* clip space */
    float    u, v;
    float    u1, v1;
    uint32_t rgba;            /* en memoria: R, G, B, A */
} ps3_rvert_t;
#pragma pack(pop)

/* Factores de blending: los mismos valores que PVR_BLEND_* (dc/pvr.h). */
enum {
    RSX_BLEND_ZERO = 0, RSX_BLEND_ONE, RSX_BLEND_DESTCOLOR, RSX_BLEND_INVDESTCOLOR,
    RSX_BLEND_SRCALPHA, RSX_BLEND_INVSRCALPHA, RSX_BLEND_DESTALPHA, RSX_BLEND_INVDESTALPHA
};

/* Comparaciones de profundidad, ya traducidas al depth buffer del RSX
 * (menor = mas cerca). */
enum {
    RSX_DEPTH_NEVER = 0, RSX_DEPTH_LESS, RSX_DEPTH_EQUAL, RSX_DEPTH_LEQUAL,
    RSX_DEPTH_GREATER, RSX_DEPTH_NOTEQUAL, RSX_DEPTH_GEQUAL, RSX_DEPTH_ALWAYS
};

/* Textura ya convertida y subida al RSX (A8R8G8B8 lineal). Opaca. */
typedef struct ps3_rtex ps3_rtex_t;

typedef struct {
    ps3_rtex_t *tex;         /* NULL = sin textura (solo color) */
    uint8_t blend;           /* 0 = opaco */
    uint8_t src, dst;        /* RSX_BLEND_* */
    uint8_t depth_func;      /* RSX_DEPTH_* */
    uint8_t depth_write;     /* 0/1 */
    uint8_t alpha_test;      /* 0/1: descarta texels con alfa 0 */
    uint8_t filter;          /* 0 = nearest, 1 = bilinear */
    uint8_t clamp;           /* bit0/1 = clamp en u/v, bit2/3 = espejo en u/v */
} ps3_rstate_t;

/* Crea una textura a partir de w*h pixeles 0xAARRGGBB (nativos). Devuelve
 * NULL si no hay memoria. */
ps3_rtex_t *PS3_RSX_TexCreate(int w, int h, const uint32_t *argb);

/* La libera cuando el RSX ya no puede estar leyendola (unos frames despues). */
void PS3_RSX_TexRelease(ps3_rtex_t *t);

/* 0 = ok. Llamar despues de PS3_Video_Init. */
int  PS3_RSX_Init(void);
void PS3_RSX_Shutdown(void);
int  PS3_RSX_Ready(void);

/* Abre el frame del motor: viewport 4:3 centrado, limpieza con el color de
 * fondo del PVR (0xRRGGBB), shaders y estado por defecto. */
void PS3_RSX_BeginScene(uint32_t bg_rgb);

/* Triangulos sueltos (n multiplo de 3) con un estado. */
/* Presentacion de un frame (flip), llamadas desde pvr_scene_finish. */
void PS3_RSX_PresentBegin(void);
void PS3_RSX_PresentEnd(void);

void PS3_RSX_DrawTris(const ps3_rstate_t *st, const ps3_rvert_t *v, int n);

/* Viewport en pantalla: el motor dibuja en 640x480. */
#define RSX_GAME_W  640.0f
#define RSX_GAME_H  480.0f

/* Gamma (compat/ps3_pvr.c). Niveles: 0.50 0.75 OFF 1.25 1.50 1.75 2.00 2.50 3.00 */
#ifndef PS3_GAMMA_LEVELS
#define PS3_GAMMA_LEVELS 9
#define PS3_GAMMA_OFF    2
#endif
void PS3_PVR_SetGamma(int level);
void PS3_PVR_SetRawTexture(void *ptr);

#endif /* PS3_RSX_H */
