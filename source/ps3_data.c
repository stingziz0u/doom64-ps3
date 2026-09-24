/* ps3_data.c -- etapa 1a: cargar los datos del juego y validar el endianness.
 *
 * Lo que tiene que dar (valores medidos sobre los archivos que genera el
 * wadtool a partir del ROM 1.1; si la PS3 loguea otra cosa, el endianness
 * esta mal):
 *
 *   pow2.wad        PWAD  numlumps=1495  infotableofs=3987880  size=4011800
 *   alt.wad         PWAD  numlumps=311   infotableofs=797768   size=802744
 *   maps/map01.wad  IWAD  numlumps=14    infotableofs=112576   size=112800
 *
 * y en map01 los lumps clasicos: MAP01, THINGS, LINEDEFS, SIDEDEFS,
 * VERTEXES, SEGS, SSECTORS, NODES...
 */

#include <stdio.h>
#include <string.h>

#include "ps3_data.h"
#include "ps3_wad.h"
#include "ps3_map.h"
#include "ps3_log.h"

static void report(const char *label, const char *path, int show_lumps)
{
    wadfile_t w;
    char nm[9];
    int bad, i, n;

    if (PS3_Wad_Load(&w, path) < 0) {
        ps3_logf("[wad] %s: FALLO la carga", label);
        return;
    }

    ps3_logf("[wad] %-14s %s numlumps=%-6d infotableofs=%-9d size=%u",
             label, w.ident, w.numlumps, w.infotableofs, (unsigned)w.size);

    bad = PS3_Wad_Validate(&w);
    if (bad == 0) {
        ps3_logf("[wad]   coherencia OK: los %d lumps caen dentro del archivo",
                 w.numlumps);
    } else {
        ps3_logf("[wad]   *** %d lumps FUERA DE RANGO -- endianness mal ***", bad);
    }

    n = w.numlumps < show_lumps ? w.numlumps : show_lumps;
    for (i = 0; i < n; i++) {
        PS3_Wad_LumpName(&w.lumps[i], nm);
        ps3_logf("[wad]   [%3d] pos=%-9d size=%-8d %s%s",
                 i, WAD_FILEPOS(w.lumps[i].filepos), w.lumps[i].size, nm,
                 (w.lumps[i].filepos & WAD_COMPRESSED_BIT) ? "  (comprimido)" : "");
    }

    PS3_Wad_Free(&w);
}

void PS3_Data_Probe(const char *basedir)
{
    char path[320];

    ps3_log("[wad] --- carga de datos y validacion de endianness ---");

    snprintf(path, sizeof(path), "%s/pow2.wad", basedir);
    report("pow2.wad", path, 6);

    snprintf(path, sizeof(path), "%s/alt.wad", basedir);
    report("alt.wad", path, 4);

    /* El mapa es el que mas importa: si los lumps de map01 salen con los
     * nombres correctos, el camino completo (header -> tabla -> nombres)
     * quedo bien. */
    snprintf(path, sizeof(path), "%s/maps/map01.wad", basedir);
    report("maps/map01", path, 14);

    ps3_log("[wad] --- fin de la validacion ---");

    /* Etapa 1b: leer las estructuras internas del mapa. */
    PS3_Map_Probe(basedir, 1);
}
