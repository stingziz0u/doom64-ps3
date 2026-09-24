/* compat/kos.h -- shim de KallistiOS para PS3.
 *
 * El motor (doom64-dc) hace #include <kos.h> desde doomdef.h, y de ahi baja a
 * todos los .c. En vez de parchear cada archivo, este header se pone primero
 * en el include path y satisface lo que el motor realmente usa. Los fuentes
 * del motor quedan sin tocar.
 *
 * Solo entra lo que el motor usa de verdad -- el inventario completo de
 * simbolos KOS (sin contar PVR) son unos 30, y casi todos mapean 1:1.
 */

#ifndef PS3_COMPAT_KOS_H
#define PS3_COMPAT_KOS_H

#include <stdint.h>
/* Escribe un uint32 en little-endian byte a byte (nombres de lump armados como int). */
#define PS3_PUT_LE32(p, v) do { uint32_t _pv = (uint32_t)(v); ((uint8_t *)(p))[0] = (uint8_t)_pv; ((uint8_t *)(p))[1] = (uint8_t)(_pv >> 8); ((uint8_t *)(p))[2] = (uint8_t)(_pv >> 16); ((uint8_t *)(p))[3] = (uint8_t)(_pv >> 24); } while (0)
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <malloc.h>

/* ---- tipos de KOS ---- */
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;

/* ---- filesystem ----
 * KOS usa descriptores propios; en PS3 son descriptores POSIX. -1 = error en
 * los dos, asi que la semantica se conserva. */
typedef int file_t;

#ifndef FILEHND_INVALID
#define FILEHND_INVALID (-1)
#endif

/* fs_open traduce los puntos de montaje del Dreamcast (/pc, /cd, /rd) a la
 * carpeta del juego en la PS3. El motor los usa literales: R_InitSymbols
 * abre "/pc/symbols.raw" y despues deja fnpre = "/pc" para todo lo demas. */
int ps3_fs_open(const char *path, int mode);
void PS3_FS_SetBase(const char *dir);
#define fs_open(p, m)        ps3_fs_open((p), (m))
#define fs_close(h)          close((h))
#define fs_read(h, b, n)     read((h), (b), (n))
#define fs_write(h, b, n)    write((h), (b), (n))
#define fs_seek(h, o, w)     lseek((h), (o), (w))
#define fs_tell(h)           lseek((h), 0, SEEK_CUR)

/* O_DIR no existe en POSIX; el motor lo usa para abrir directorios. */
#ifndef O_DIR
#define O_DIR 0
#endif

size_t fs_total(file_t h);
/* Firma de KOS: devuelve el tamano (o -1) y deja el buffer en *out.
 * (La primera version del shim la tenia al reves -- void *fs_load(path,
 * size_t *sz) -- y como el motor compila con -w, r_data.c y s_sound.c
 * hubieran pasado &puntero donde se esperaba &tamano sin ningun aviso.) */
ssize_t fs_load(const char *path, void **out);

/* dirent de KOS. El motor lo usa para el menu de saves (FileState[200]) y
 * para listar mapas extra. Se mantiene el layout de KOS: name fijo, size, y
 * un flag de atributos. */
#define MAX_FN_LEN 256
typedef struct {
    char   name[MAX_FN_LEN];
    int    size;
    int    time;
    int    attr;
} dirent_t;

typedef struct { int fd; dirent_t ent; } ps3_dir_t;
dirent_t *fs_readdir(file_t h);

/* ---- debug ----
 * dbgio_printf va al log del port (66 call sites en el motor). */
void dbgio_printf(const char *fmt, ...);

#include "dc/vector.h"
#include "dc/pvr.h"
#include "dc/matrix.h"
#include "dc/fmath.h"
#include "dc/maple.h"
#include "kos/thread.h"
#include "dc/sound/sound.h"

/* ------------------------------------------------------------------------
 * ENDIANNESS: los datos del juego son SIEMPRE little-endian.
 *
 * Estos son los mismos macros que tenia DOOM64-RE (doomdef.h:576-591) y que
 * doom64-dc vacio porque el Dreamcast es little-endian y no los necesitaba.
 * En los puntos de lectura quedaron los parentesis huerfanos --
 * "li->x = (ml->x);" -- que son la marca de donde iban.
 *
 * OJO con el nombre: en DOOM64-RE el #ifdef __BIG_ENDIAN__ hace lo CONTRARIO
 * de lo que sugiere (con la macro definida NO swapea). El build real de N64,
 * que es big-endian, usaba la rama de abajo. La macro significaba "los datos
 * ya vienen en mi endianness", no "mi CPU es big-endian".
 *
 * Aca se decide por el endianness REAL del compilador, asi el mismo codigo es
 * correcto en PS3 (swapea) y en x86 (no hace nada), y se puede probar en la
 * PC antes de compilar para la consola.
 * ------------------------------------------------------------------------ */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  #define LONGSWAP(x)     ((int)__builtin_bswap32((unsigned int)(x)))
  #define LITTLESHORT(x)  ((short)__builtin_bswap16((unsigned short)(x)))
  #define LITTLEUSHORT(x) ((unsigned short)__builtin_bswap16((unsigned short)(x)))
#else
  #define LONGSWAP(x)     (x)
  #define LITTLESHORT(x)  (x)
  #define LITTLEUSHORT(x) (x)
#endif

/* KOS define F_PI como float; el motor lo usa en r_lights y r_phase3. */
#ifndef F_PI
#define F_PI 3.1415926f
#endif

/* audio de KOS */
typedef uint32_t sfxhnd_t;
#define SFXHND_INVALID 0

/* dbglog de KOS: el motor solo sube el nivel; no hace falta. */
#define DBG_INFO 6
static inline void dbglog_set_level(int l) { (void)l; }

/* Relojes de KOS (compat/ps3_system.c). Sin prototipo el motor los llamaba
 * como "int f()": en PPC64 eso corta el valor de 64 bits a 32. */
uint64_t perf_cntr_timer_ns(void);
uint32_t rtc_unix_secs(void);

#endif /* PS3_COMPAT_KOS_H */
