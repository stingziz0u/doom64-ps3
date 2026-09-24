/* ps3_log.h -- logging a archivo, la unica fuente de diagnostico en PS3.
 *
 * Doom64Cell -- port de Doom 64 (doom64-dc / DOOM64-RE) a PS3 homebrew.
 */

#ifndef PS3_LOG_H
#define PS3_LOG_H

/* Ruta del log. USRDIR siempre es escribible; /dev_hdd0/data/ se prueba aparte. */
#define PS3_USRDIR      "/dev_hdd0/game/DOOM64CEL/USRDIR"
#define PS3_DATADIR     "/dev_hdd0/data/doom64"

void ps3_log_init(void);
void ps3_log(const char *msg);
void ps3_logf(const char *fmt, ...);
void ps3_log_shutdown(void);

#endif /* PS3_LOG_H */
