/* ps3_platform_stub.c -- los simbolos de i_main.c, s_sound.c y sndwav.c.
 *
 * Esos tres archivos son la capa de plataforma del motor (video, input, VMU,
 * rumble, audio) y son exactamente lo que hay que reescribir para PS3, asi
 * que no se compilan. El resto del motor los referencia igual, y esas
 * referencias se resuelven aca.
 *
 * A medida que avancen las etapas, cada grupo se va reemplazando por la
 * implementacion de verdad:
 *   I_*   video/input/saves  -> etapa 2 (ioPad, HDD) y etapa 3 (RSX)
 *   S_*   audio              -> etapa 2 (audioPort)
 *
 * Igual que en ps3_kos_stub.c, cada stub loguea UNA vez al ser llamado. El
 * log termina siendo un mapa de que partes del motor se ejercitan de verdad.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "doomdef.h"
#include "../ps3_log.h"

#define STUB_ONCE(etapa)                                        \
    do {                                                        \
        static int _seen = 0;                                   \
        if (!_seen) {                                           \
            _seen = 1;                                          \
            ps3_logf("[stub] %s()  (pendiente: %s)", __func__, etapa); \
        }                                                       \
    } while (0)

#define STUB2  STUB_ONCE("etapa 2")
#define STUB3  STUB_ONCE("etapa 3")

/* ================================================================== */
/* Globales de i_main.c                                                */
/* ================================================================== */

dirent_t __attribute__((aligned(32))) FileState[200];
int32_t  FilesUsed = 0;

uint8_t *Pak_Data   = NULL;
int32_t  Pak_Memory = 0;
int32_t  Pak_Size   = 0;

boolean  disabledrawing = false;
volatile int32_t drawsync1 = 0;
volatile int32_t drawsync2 = 0;
volatile int32_t vsync     = 0;

int early_error = 0;
int last_Ltrig  = 0;
int last_Rtrig  = 0;

mapped_buttons_t ingame_mapping;

/* sounds[] y soundscale: ahora los define el s_sound.c del motor. */

/* ================================================================== */
/* I_Error directo.
 * doomdef.h define I_Error como macro hacia __I_Error, pero algunos sitios
 * del motor lo llaman como funcion. Hay que deshacer la macro antes.        */
/* ================================================================== */

#undef I_Error
void __attribute__((noreturn)) I_Error(char *error, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, error);
    vsnprintf(buf, sizeof(buf), error, ap);
    va_end(ap);

    __I_Error("I_Error", "%s", buf);
    for (;;) { }
}

/* ================================================================== */
/* ETAPA 2/3: video y frame                                            */
/* ================================================================== */

void I_Init(void)       { STUB2; }
/* i_main.c:1032, tal cual. NextFrameIdx es el reloj del cache de texturas
 * (z_zone lo usa para envejecer bloques PU_CACHE) y globallump/globalcm son
 * la "textura actual" del renderer: resetearlos fuerza a reenviar el header
 * en el primer poligono del frame. */
extern uint32_t NextFrameIdx;
extern int globallump, globalcm;
void I_ClearFrame(void)
{
    NextFrameIdx += 1;
    globallump = -1;
    globalcm = -2;
}
/* En el Dreamcast espera la senal de vblank. Aca el flip lo maneja main.c. */
void I_DrawFrame(void)  { }


/* ================================================================== */
/* ETAPA 2: input, rumble y VMU                                        */
/* ================================================================== */

void I_ParseMappingFile(char *f)            { (void)f; STUB2; }

void I_VMUFB(int force)                     { (void)force; STUB2; }
void I_VMUUpdateFace(uint8_t *img, int f)   { (void)img; (void)f; STUB2; }

/* Mando, vibracion, saves, transiciones y tiempo: compat/ps3_system.c */

/* ================================================================== */
/* ETAPA 2: audio                                                      */
/* ================================================================== */

/* S_*: ahora es el s_sound.c del motor, sobre compat/ps3_audio.c */

/* ================================================================== */
/* Helpers que en Dreamcast eran assembly del SH4                      */
/* ================================================================== */

/* array_fast_copy.S del repo original: copia n VERTICES de 32 bytes, de
 * src[i] a dst[i] (los arrays son de punteros; lo que se copia es a lo que
 * apuntan). La version anterior copiaba los punteros: los pisos partidos
 * que se iluminan con luces dinamicas (disparos, proyectiles) quedaban con
 * vertices basura -> triangulos gigantes en el frame del disparo. */
void array_fast_cpy(void **dst, const void **src, size_t n)
{
    size_t i;
    if (!dst || !src) return;
    for (i = 0; i < n; i++)
        memcpy(dst[i], src[i], 32);
}

/* Copia de un bloque de 32 bytes (el tamano de una store queue del SH4). */
void single_fast_cpy(void *dst, const void *src)
{
    if (dst && src) memcpy(dst, src, 32);
}

/* ================================================================== */
/* Tiempo (KOS)                                                        */
/* ================================================================== */


/* Cola de mensajes del render en el port de Dreamcast. */
atomic_int rdpmsg;
