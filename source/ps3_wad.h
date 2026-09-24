/* ps3_wad.h -- lector de WAD para Doom 64, con el endianness resuelto.
 *
 * ---------------------------------------------------------------------------
 * EL PUNTO CLAVE DE TODO ESTE ARCHIVO:
 *
 * Los .wad que genera el wadtool son SIEMPRE little-endian: los escribe en la
 * PC con fwrite de structs crudos, sin ninguna conversion. Y el IWAD original
 * dentro del ROM de N64 tambien es little-endian (se hizo con herramientas de
 * PC y se embebio tal cual en el cartucho).
 *
 * La PS3 es big-endian. Por lo tanto estos swaps son INCONDICIONALES.
 *
 * NO poner esto detras de un #ifdef __BIG_ENDIAN__. En DOOM64-RE esa macro
 * hace lo contrario de lo que su nombre sugiere:
 *
 *     #ifdef __BIG_ENDIAN__
 *     #define LONGSWAP(x)   (x)          // NO swapea
 *     #else
 *     #define LONGSWAP(x)   LongSwap(x)  // SI swapea
 *     #endif
 *
 * El build real de N64 (que es big-endian) usa la rama de abajo, o sea que
 * __BIG_ENDIAN__ NO estaba definido. La macro significa "los datos ya vienen
 * en mi endianness", no "mi CPU es big-endian". Definirla para PS3 porque
 * suena correcto rompe todo, y el sintoma son mapas corruptos, no un error.
 * ---------------------------------------------------------------------------
 */

#ifndef PS3_WAD_H
#define PS3_WAD_H

#include <stddef.h>

/* Lectura little-endian byte a byte desde el buffer del archivo.
 *
 * Se hace asi y no casteando structs ni con __builtin_bswap por tres motivos:
 *   1. No depende del endianness de la plataforma: el mismo codigo da el
 *      mismo resultado en x86 y en PS3, asi que lo que se prueba en la PC es
 *      exactamente lo que corre en la consola.
 *   2. No depende de la alineacion del puntero. En PPC un acceso desalineado
 *      a int no perdona.
 *   3. No depende de que el compilador no meta padding en los structs.
 *
 * Es tambien lo que dice la experiencia previa: el header de un WAD es LE,
 * armarlo byte a byte, NO castear. */
static inline int wad_rd_long(const unsigned char *p)
{
    return (int)((unsigned int)p[0]
               | ((unsigned int)p[1] << 8)
               | ((unsigned int)p[2] << 16)
               | ((unsigned int)p[3] << 24));
}

static inline short wad_rd_short(const unsigned char *p)
{
    return (short)(unsigned short)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
}

/* Layout EN DISCO. Los offsets se usan con wad_rd_long(); los structs estan
 * solo como documentacion del formato. */
#define WADINFO_SIZE        12   /* char ident[4]; int numlumps; int infotableofs */
#define LUMPINFO_SIZE       16   /* int filepos; int size; char name[8]           */
#define LUMPINFO_OFS_POS     0
#define LUMPINFO_OFS_SIZE    4
#define LUMPINFO_OFS_NAME    8

typedef struct {
    int  filepos;             /* bit 31 = lump comprimido (Doom 64) */
    int  size;
    char name[8];             /* puede no terminar en NUL */
} lumpinfo_t;

#define WAD_COMPRESSED_BIT  0x80000000
#define WAD_FILEPOS(x)      ((x) & 0x7fffffff)

typedef struct {
    char           path[256];
    unsigned char *data;      /* archivo entero en memoria */
    size_t         size;
    char           ident[5];  /* con NUL, para loguear */
    int            numlumps;
    int            infotableofs;
    lumpinfo_t    *lumps;     /* ya convertidos a big-endian */
} wadfile_t;

/* 0 = ok, <0 = error (el motivo va al log). */
int  PS3_Wad_Load(wadfile_t *w, const char *path);
void PS3_Wad_Free(wadfile_t *w);

/* Devuelve la cantidad de lumps cuyo rango cae fuera del archivo.
 * 0 = el endianness esta bien. Cualquier otra cosa = esta mal. */
int  PS3_Wad_Validate(const wadfile_t *w);

/* Copia el nombre del lump a un buffer de 9 bytes, con NUL y sin el bit de
 * comprimido en el primer caracter. */
void PS3_Wad_LumpName(const lumpinfo_t *l, char out[9]);

int  PS3_Wad_FindLump(const wadfile_t *w, const char *name);

#endif /* PS3_WAD_H */
