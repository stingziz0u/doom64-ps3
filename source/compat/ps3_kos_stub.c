/* ps3_kos_stub.c -- implementacion del shim de KallistiOS para PS3.
 *
 * Los headers de compat/ declaran la superficie de KOS que usa el motor; aca
 * estan las implementaciones. Se dividen en tres grupos:
 *
 *   REAL     -- ya funcionan (filesystem, log, error fatal, matematica).
 *   ETAPA 2  -- audio, input, threads: se implementan cuando entre la capa
 *               de plataforma.
 *   ETAPA 3  -- todo el PVR: se reemplaza por el backend RSX.
 *
 * Los stubs de etapa 2/3 no hacen nada, pero LOGUEAN la primera vez que se
 * los llama. Eso convierte el log en un mapa de que partes del motor se
 * estan ejercitando realmente, que es justo lo que hace falta para decidir
 * el orden del trabajo que viene.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "kos.h"
#include "dc/pvr.h"
#include "dc/matrix.h"
#include "dc/maple.h"
#include "kos/thread.h"
#include "kos/worker_thread.h"
#include "dc/sound/sound.h"
#include "dc/sound/stream.h"

#include "../ps3_log.h"

/* Loguea una sola vez por sitio. Sin esto un stub llamado por frame llena el
 * log en segundos -- la leccion de los contadores de audio de CrispyCell. */
#define STUB_ONCE(etapa)                                        \
    do {                                                        \
        static int _seen = 0;                                   \
        if (!_seen) {                                           \
            _seen = 1;                                          \
            ps3_logf("[stub] %s()  (pendiente: %s)", __func__, etapa); \
        }                                                       \
    } while (0)

#define STUB2  STUB_ONCE("etapa 2")
#define STUB3  STUB_ONCE("etapa 3 - backend RSX")

/* ================================================================== */
/* REAL: filesystem                                                    */
/* ================================================================== */

size_t fs_total(file_t h)
{
    off_t cur = lseek(h, 0, SEEK_CUR);
    off_t end = lseek(h, 0, SEEK_END);
    lseek(h, cur, SEEK_SET);
    return (size_t)end;
}

/* ------------------------------------------------------------------ */
/* Puntos de montaje del Dreamcast -> USRDIR                            */
/* ------------------------------------------------------------------ */
static char s_fs_base[256] = PS3_USRDIR;

void PS3_FS_SetBase(const char *dir)
{
    snprintf(s_fs_base, sizeof(s_fs_base), "%s", dir);
}

int ps3_fs_open(const char *path, int mode)
{
    char buf[512];
    if (path && path[0] == '/' && path[3] == '/' &&
        (!strncmp(path, "/pc/", 4) || !strncmp(path, "/cd/", 4) || !strncmp(path, "/rd/", 4))) {
        snprintf(buf, sizeof(buf), "%s/%s", s_fs_base, path + 4);
        path = buf;
    }
    return open(path, mode);
}

ssize_t fs_load(const char *path, void **out)
{
    file_t h;
    size_t n;
    void  *buf;

    if (out) *out = NULL;

    h = fs_open(path, O_RDONLY);
    if (h < 0) {
        ps3_logf("[fs] fs_load: no se pudo abrir %s", path);
        return -1;
    }

    n = fs_total(h);
    buf = malloc(n ? n : 1);
    if (!buf) {
        ps3_logf("[fs] fs_load: sin memoria para %s (%u bytes)", path, (unsigned)n);
        fs_close(h);
        return -1;
    }

    if ((size_t)read(h, buf, n) != n) {
        ps3_logf("[fs] fs_load: lectura incompleta de %s", path);
        free(buf);
        fs_close(h);
        return -1;
    }

    fs_close(h);
    if (out) *out = buf; else free(buf);
    return (ssize_t)n;
}

dirent_t *fs_readdir(file_t h)
{
    (void)h;
    STUB2;
    return NULL;
}

/* ================================================================== */
/* REAL: log y error fatal                                             */
/* ================================================================== */

void dbgio_printf(const char *fmt, ...)
{
    char buf[512];
    size_t n;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* El motor mete '\n' en casi todos sus dbgio_printf; ps3_log ya agrega
     * el salto, asi que se saca para no dejar lineas en blanco. */
    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';

    if (n > 0)
        ps3_logf("[eng] %s", buf);
}

/* I_Error del motor. Por ahora deja constancia en el log y sale; dibujar el
 * mensaje en pantalla es el pendiente que arrastran los tres ports. */
void __attribute__((noreturn)) __I_Error(const char *funcname, char *error, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, error);
    vsnprintf(buf, sizeof(buf), error, ap);
    va_end(ap);

    ps3_logf("[FATAL] I_Error en %s(): %s", funcname ? funcname : "?", buf);
    ps3_log_shutdown();

    exit(1);
}

/* ================================================================== */
/* ETAPA 2: threads y sincronizacion                                   */
/* ================================================================== */

kthread_t *thd_create_ex(kthread_attr_t *attr, void *(*routine)(void *), void *param)
{ (void)attr; (void)routine; (void)param; STUB2; return NULL; }

kthread_t *thd_create(int detach, void *(*routine)(void *), void *param)
{ (void)detach; (void)routine; (void)param; STUB2; return NULL; }

int  thd_join(kthread_t *t, void **v) { (void)t; (void)v; STUB2; return 0; }
void thd_pass(void)                   { STUB2; }
int  thd_sleep(int ms)                { usleep((useconds_t)ms * 1000); return 0; }

/* Mutex: por ahora contadores. Con un solo thread de juego alcanza; cuando
 * entre el thread de audio en la etapa 2 pasan a sys_lwmutex. */
int mutex_init(mutex_t *m, int type) { (void)type; if (m) m->locked = 0; return 0; }
int mutex_destroy(mutex_t *m)        { (void)m; return 0; }
int mutex_lock(mutex_t *m)           { if (m) m->locked = 1; return 0; }
int mutex_unlock(mutex_t *m)         { if (m) m->locked = 0; return 0; }
int mutex_trylock(mutex_t *m)        { if (m) m->locked = 1; return 0; }

int sem_init(semaphore_t *s, int c)  { if (s) s->count = c; return 0; }
int sem_destroy(semaphore_t *s)      { (void)s; return 0; }
int sem_wait(semaphore_t *s)         { (void)s; STUB2; return 0; }
int sem_signal(semaphore_t *s)       { (void)s; STUB2; return 0; }

kthread_worker_t *thd_worker_create_ex(const char *n, int p, void (*r)(void *), void *d)
{ (void)n; (void)p; (void)r; (void)d; STUB2; return NULL; }
void thd_worker_wakeup(kthread_worker_t *w)  { (void)w; STUB2; }
void thd_worker_destroy(kthread_worker_t *w) { (void)w; STUB2; }
kthread_t *thd_worker_get_thread(kthread_worker_t *w) { (void)w; return NULL; }

/* ================================================================== */
/* ETAPA 2: input (Maple -> ioPad)                                     */
/* ================================================================== */

/* D_SplashScreen pregunta si hay un mando del Dreamcast: en la PS3 el DS3 lo
 * maneja compat/ps3_system.c, asi que siempre "hay mando" (si no, se queda
 * en la pantalla de WARNING para siempre). */
maple_device_t *maple_enum_type(int n, uint32_t func)
{
    static maple_device_t fake_controller;
    (void)n;
    if (func == MAPLE_FUNC_CONTROLLER) return &fake_controller;
    return NULL;
}

maple_device_t *maple_enum_dev(int p, int u)
{ (void)p; (void)u; STUB2; return NULL; }

int maple_dev_status(maple_device_t *d) { (void)d; STUB2; return 0; }

/* ================================================================== */
/* ETAPA 2: audio                                                      */
/* ================================================================== */

/* snd_sfx_*: ahora en compat/ps3_audio.c */
int      snd_init(void)                          { return 0; }
void     snd_shutdown(void)                      { }

int  snd_stream_init(void)                       { STUB2; return 0; }
void snd_stream_shutdown(void)                   { STUB2; }
snd_stream_hnd_t snd_stream_alloc(void *cb, int b) { (void)cb; (void)b; STUB2; return SND_STREAM_INVALID; }
void snd_stream_destroy(snd_stream_hnd_t h)      { (void)h; STUB2; }
void snd_stream_stop(snd_stream_hnd_t h)         { (void)h; STUB2; }
int  snd_stream_start(snd_stream_hnd_t h, uint32_t f, int s)
{ (void)h; (void)f; (void)s; STUB2; return 0; }
int  snd_stream_poll(snd_stream_hnd_t h)         { (void)h; return 0; }
void snd_stream_volume(snd_stream_hnd_t h, int v){ (void)h; (void)v; STUB2; }

/* ================================================================== */
/* ETAPA 3: las matrices viven en ps3_matrix.c y todo el PVR (el        */
/* emulador del TA) en ps3_pvr.c.                                       */
/* ================================================================== */

/* Empaqueta dos coordenadas UV como half-floats en un uint32 (lo que hace el
 * TA del PVR). El backend RSX va a usar floats normales, pero el motor arma
 * los vertices con este formato. */
uint32_t ps3_pvr_pack_uv(float u, float v)
{
    union { float f; uint32_t i; } cu, cv;
    cu.f = u; cv.f = v;
    return (cu.i & 0xffff0000u) | (cv.i >> 16);
}
