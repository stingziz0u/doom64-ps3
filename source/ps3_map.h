/* ps3_map.h -- carga de un mapa de Doom 64 (etapa 1b).
 *
 * Lee los lumps de maps/mapNN.wad y valida que todos los tipos salgan bien
 * en big-endian. Todavia NO arma las estructuras del motor (vertex_t, line_t,
 * sector_t...) -- eso es la etapa 1c, cuando entre p_setup.c de verdad. Aca
 * solo se comprueba que sabemos leer cada struct de disco.
 *
 * NOTA SOBRE COMO SE LEE (importante):
 * Los campos NO se leen casteando un struct sobre el buffer. Se arman byte a
 * byte en orden little-endian explicito. Dos razones:
 *
 *   1. Alineacion/padding. maplinedef_t tiene un 'int' entre shorts; castear
 *      depende de que el compilador no meta padding y de que el puntero este
 *      alineado. En PPC un acceso desalineado a int no perdona.
 *   2. El codigo queda identico en x86 y en PS3, asi que lo que se prueba en
 *      la PC es exactamente lo que corre en la consola.
 */

#ifndef PS3_MAP_H
#define PS3_MAP_H

/* Orden de los lumps dentro del wad de un mapa (doomdata.h del motor). */
enum {
    ML_LABEL,
    ML_THINGS,
    ML_LINEDEFS,
    ML_SIDEDEFS,
    ML_VERTEXES,
    ML_SEGS,
    ML_SSECTORS,
    ML_NODES,
    ML_SECTORS,
    ML_REJECT,
    ML_BLOCKMAP,
    ML_LEAFS,
    ML_LIGHTS,
    ML_MACROS,
    ENDOFWAD
};

/* Tamanos EN DISCO de cada struct de mapa. No usar sizeof() de los typedef
 * del motor para esto: lo que importa es el layout del archivo. */
#define MAPVERTEX_SIZE     8    /* int x, y                                  */
#define MAPLINEDEF_SIZE   16    /* short v1,v2; int flags; short sp,tag,sn[2]*/
#define MAPSIDEDEF_SIZE   12    /* 6 shorts                                  */
#define MAPSEG_SIZE       12    /* 6 shorts                                  */
#define MAPSUBSECTOR_SIZE  4    /* 2 shorts                                  */
#define MAPNODE_SIZE      28    /* 4 + 8 + 2 shorts                          */
#define MAPSECTOR_SIZE    24    /* 12 shorts                                 */
#define MAPTHING_SIZE     14    /* 7 shorts                                  */

/* Carga mapNN.wad desde basedir y loguea el resumen + valores de control.
 * 0 = ok, <0 = error. */
int PS3_Map_Probe(const char *basedir, int mapnum);

#endif /* PS3_MAP_H */
