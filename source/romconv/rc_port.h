/* rc_port.h -- adaptacion del wadtool de doom64-dc para correr DENTRO del
 * juego en la PS3 (Makefile: se incluye con -include antes de cada rc_*.c).
 *
 * El wadtool original es un programa de PC (x86, little-endian) que se corre
 * una vez para generar los datos del port de Dreamcast desde la ROM de N64.
 * Aca lo corremos en el primer arranque. Tres cosas cambian:
 *
 *  1. exit(): en un programa de PC termina el proceso; aca mataria el juego.
 *     Se convierte en un longjmp de vuelta a PS3_RomConv_Run(), que informa
 *     el error en pantalla y en el log.
 *  2. fprintf(stderr)/printf: van al log del juego.
 *  3. Endianness: el wadtool asume un host little-endian (lee la cabecera del
 *     WAD de la ROM "a mano" y escribe cabeceras y directorios en el orden del
 *     host). En la PS3 (big-endian) esos accesos pasan por rc_le32/rc_le16.
 *     Los datos del N64 (big-endian) los lee con SwapShort, que en un host
 *     big-endian pasa a ser la identidad.
 *
 * Los simbolos de estos archivos (DecodeD64, lumpinfo, SwapShort...) chocan
 * con los del motor: el Makefile los junta con `ld -r` y deja global solo
 * PS3_RomConv_* (objcopy --keep-global-symbol).
 */
#ifndef RC_PORT_H
#define RC_PORT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>

extern jmp_buf rc_fail_jmp;
void rc_log(const char *fmt, ...);
void rc_vlog(const char *fmt, va_list ap);
void rc_progress(int step, const char *what);
void rc_subprogress(int done, int total);   /* avance dentro del paso actual */

#define exit(code)                  longjmp(rc_fail_jmp, 1)
#define fprintf(stream, ...)        rc_log(__VA_ARGS__)
#define vfprintf(stream, fmt, ap)   rc_vlog(fmt, ap)
#define printf(...)                 rc_log(__VA_ARGS__)

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define RC_HOST_BE 1
#else
#define RC_HOST_BE 0
#endif

/* host <-> little-endian (la misma operacion en los dos sentidos) */
static inline uint32_t rc_le32(uint32_t v)
{
#if RC_HOST_BE
	return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
#else
	return v;
#endif
}

static inline uint16_t rc_le16(uint16_t v)
{
#if RC_HOST_BE
	return (uint16_t)((v >> 8) | (v << 8));
#else
	return v;
#endif
}

/* fwrite de un entero de 32 bits en little-endian (cabeceras de WAD) */
static inline size_t rc_fwrite_le32(const void *p, FILE *fd)
{
	uint32_t v = rc_le32(*(const uint32_t *)p);
	return fwrite(&v, 1, 4, fd);
}

#endif /* RC_PORT_H */
