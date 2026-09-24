/* ps3_engine_glue.c -- los simbolos que normalmente aporta i_main.c.
 *
 * i_main.c es la capa de plataforma del motor (video, input, VMU, loop de
 * frames) y es justamente lo que hay que reescribir para PS3. En vez de
 * parchearlo, no se compila: los simbolos que el resto del motor necesita de
 * el se definen aca, y se van llenando a medida que avanzan las etapas.
 */

#include <stdint.h>
#include <stddef.h>

#include "kos.h"

/* ------------------------------------------------------------------ */
/* Contador de frames.
 * z_zone lo usa para el aging de bloques PU_CACHE y p_spec para saber
 * cuando purgar sprites. Lo incrementa el loop principal. */
uint32_t NextFrameIdx = 0;

/* PS3_CountMobjs / PS3_GetSpawnCount viven en ps3_game.c, que incluye los
 * headers del motor. */

/* NOTA: 'backres' (o_ad675382a0ccc360672c24686a0f93ee) NO se define aca.
 * Lo define p_user.c:658, que ya entra al build. Definirlo tambien aca daba
 * simbolo duplicado en el link. Quien lo LLENA es ps3_engine.c, calculando
 * el MD5 de warn3.dt -- ver el comentario largo de ese archivo. */
