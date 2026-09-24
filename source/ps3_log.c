/* ps3_log.c -- logging a archivo.
 *
 * En PS3 no hay consola: si algo falla el sintoma es pantalla negra y vuelta
 * al XMB sin mensaje. El log es la unica fuente. Por eso se abre lo antes
 * posible y se hace fflush en cada linea (un crash no debe perder la ultima
 * linea, que es justamente la que dice donde se murio).
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ps3_log.h"

/* Varios hilos escriben (juego, audio): una linea a la vez. */
#ifdef __PPU__
#include <sys/mutex.h>
static sys_mutex_t s_lmtx;
static int s_lmtx_ok = 0;
static void log_lock_init(void)
{
    sys_mutex_attr_t a;
    memset(&a, 0, sizeof(a));
    a.attr_protocol  = SYS_MUTEX_PROTOCOL_PRIO;
    a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    a.attr_pshared   = SYS_MUTEX_ATTR_NOT_PSHARED;
    a.attr_adaptive  = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
    strcpy(a.name, "d64log");
    s_lmtx_ok = (sysMutexCreate(&s_lmtx, &a) == 0);
}
#define LOG_LOCK()   do { if (s_lmtx_ok) sysMutexLock(s_lmtx, 0); } while (0)
#define LOG_UNLOCK() do { if (s_lmtx_ok) sysMutexUnlock(s_lmtx); } while (0)
#else
#include <pthread.h>
static pthread_mutex_t s_lmtx = PTHREAD_MUTEX_INITIALIZER;
static void log_lock_init(void) { }
#define LOG_LOCK()   pthread_mutex_lock(&s_lmtx)
#define LOG_UNLOCK() pthread_mutex_unlock(&s_lmtx)
#endif

static FILE *s_log = NULL;

void ps3_log_init(void)
{
    if (s_log) return;

    log_lock_init();
    s_log = fopen(PS3_USRDIR "/doom64_log.txt", "w");

    /* Si USRDIR fallara, no hay nada mas que hacer: se sigue sin log. */
    if (s_log) {
        fputs("=== Doom64-PS3 v1.0 log ===\n", s_log);
        fflush(s_log);
    }
}

void ps3_log(const char *msg)
{
    if (!s_log) return;
    LOG_LOCK();
    fputs(msg, s_log);
    fputc('\n', s_log);
    fflush(s_log);   /* obligatorio: sin esto un crash se lleva la ultima linea */
    LOG_UNLOCK();
}

void ps3_logf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    ps3_log(buf);
}

void ps3_log_shutdown(void)
{
    if (!s_log) return;
    ps3_log("=== fin ===");
    fclose(s_log);
    s_log = NULL;
}
