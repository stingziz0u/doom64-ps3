/* ps3_map.c -- carga y validacion de un mapa de Doom 64. Ver ps3_map.h. */

#include <stdio.h>
#include <string.h>

#include "ps3_map.h"
#include "ps3_wad.h"
#include "ps3_log.h"

/* ------------------------------------------------------------------ */
/* Lectura little-endian byte a byte: independiente del endianness de  */
/* la plataforma y de la alineacion del puntero.                       */
/* ------------------------------------------------------------------ */

static int rd_long(const unsigned char *p)
{
    return (int)((unsigned int)p[0]
               | ((unsigned int)p[1] << 8)
               | ((unsigned int)p[2] << 16)
               | ((unsigned int)p[3] << 24));
}

static short rd_short(const unsigned char *p)
{
    return (short)(unsigned short)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
}

/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char *data;
    int                  size;
} maplump_t;

static maplump_t get_lump(const wadfile_t *w, int index)
{
    maplump_t m;
    m.data = NULL;
    m.size = 0;

    if (index < 0 || index >= w->numlumps)
        return m;

    m.data = w->data + WAD_FILEPOS(w->lumps[index].filepos);
    m.size = w->lumps[index].size;
    return m;
}

/* Cuenta elementos y avisa si el lump no divide exacto: si no divide, o el
 * struct esta mal definido o el archivo no es lo que creemos. */
static int count_of(const char *label, const maplump_t *m, int stride)
{
    int n = m->size / stride;

    if (m->size % stride != 0) {
        ps3_logf("[map]   %-10s %7d bytes / %2d = %5d  *** NO DIVIDE EXACTO (resto %d) ***",
                 label, m->size, stride, n, m->size % stride);
    } else {
        ps3_logf("[map]   %-10s %7d bytes / %2d = %5d", label, m->size, stride, n);
    }
    return n;
}

int PS3_Map_Probe(const char *basedir, int mapnum)
{
    wadfile_t w;
    char path[320];
    maplump_t verts, lines, sides, segs, ssecs, nodes, sects, things;
    int nverts, nlines, nsides, nsegs, nssecs, nnodes, nsects, nthings;
    int i;

    snprintf(path, sizeof(path), "%s/maps/map%02d.wad", basedir, mapnum);

    ps3_logf("[map] --- cargando %s ---", path);

    if (PS3_Wad_Load(&w, path) < 0) {
        ps3_log("[map] FALLO la carga del wad del mapa");
        return -1;
    }

    if (w.numlumps < ENDOFWAD) {
        ps3_logf("[map] el mapa tiene %d lumps, se esperaban %d",
                 w.numlumps, (int)ENDOFWAD);
    }

    verts  = get_lump(&w, ML_VERTEXES);
    lines  = get_lump(&w, ML_LINEDEFS);
    sides  = get_lump(&w, ML_SIDEDEFS);
    segs   = get_lump(&w, ML_SEGS);
    ssecs  = get_lump(&w, ML_SSECTORS);
    nodes  = get_lump(&w, ML_NODES);
    sects  = get_lump(&w, ML_SECTORS);
    things = get_lump(&w, ML_THINGS);

    ps3_log("[map] conteos (si alguno no divide exacto, el struct esta mal):");
    nverts  = count_of("VERTEXES", &verts,  MAPVERTEX_SIZE);
    nlines  = count_of("LINEDEFS", &lines,  MAPLINEDEF_SIZE);
    nsides  = count_of("SIDEDEFS", &sides,  MAPSIDEDEF_SIZE);
    nsegs   = count_of("SEGS",     &segs,   MAPSEG_SIZE);
    nssecs  = count_of("SSECTORS", &ssecs,  MAPSUBSECTOR_SIZE);
    nnodes  = count_of("NODES",    &nodes,  MAPNODE_SIZE);
    nsects  = count_of("SECTORS",  &sects,  MAPSECTOR_SIZE);
    nthings = count_of("THINGS",   &things, MAPTHING_SIZE);

    /* --- vertices: int en fixed 16.16 --- */
    if (nverts > 0) {
        int minx = 0, maxx = 0, miny = 0, maxy = 0;

        ps3_log("[map] primeros vertices (unidades del mapa):");
        for (i = 0; i < nverts; i++) {
            int x = rd_long(verts.data + i * MAPVERTEX_SIZE) >> 16;
            int y = rd_long(verts.data + i * MAPVERTEX_SIZE + 4) >> 16;

            if (i == 0) { minx = maxx = x; miny = maxy = y; }
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;

            if (i < 4)
                ps3_logf("[map]   v[%d] = (%d, %d)", i, x, y);
        }
        ps3_logf("[map]   bbox: x[%d..%d]  y[%d..%d]", minx, maxx, miny, maxy);
    }

    /* --- things: 7 shorts. El primero de MAP01 tiene que ser type=1,
           que es el spawn del jugador 1. --- */
    if (nthings > 0) {
        int n = nthings < 3 ? nthings : 3;
        ps3_log("[map] primeros things (x, y, z, angle, type, options, tid):");
        for (i = 0; i < n; i++) {
            const unsigned char *p = things.data + i * MAPTHING_SIZE;
            ps3_logf("[map]   (%d, %d, %d, %d, %d, %d, %d)",
                     rd_short(p), rd_short(p + 2), rd_short(p + 4),
                     rd_short(p + 6), rd_short(p + 8), rd_short(p + 10),
                     rd_short(p + 12));
        }
        {
            int type = rd_short(things.data + 8);
            if (type == 1)
                ps3_log("[map]   OK: el primer thing es type=1 (spawn del jugador)");
            else
                ps3_logf("[map]   *** el primer thing es type=%d, se esperaba 1 ***", type);
        }
    }

    /* --- sectores: 12 shorts --- */
    if (nsects > 0) {
        int n = nsects < 2 ? nsects : 2;
        ps3_log("[map] primeros sectores (floorh, ceilh, floorpic, ceilpic, colors[5], special, tag, flags):");
        for (i = 0; i < n; i++) {
            const unsigned char *p = sects.data + i * MAPSECTOR_SIZE;
            ps3_logf("[map]   (%d, %d, %d, %d, [%d %d %d %d %d], %d, %d, %d)",
                     rd_short(p), rd_short(p + 2), rd_short(p + 4), rd_short(p + 6),
                     rd_short(p + 8),  rd_short(p + 10), rd_short(p + 12),
                     rd_short(p + 14), rd_short(p + 16),
                     rd_short(p + 18), rd_short(p + 20), rd_short(p + 22));
        }
    }

    /* --- linedefs: el caso mixto (shorts + un int en el medio). Si los
           indices de vertices caen dentro de rango, el layout esta bien. --- */
    if (nlines > 0 && nverts > 0) {
        int bad_v = 0, bad_s = 0;

        for (i = 0; i < nlines; i++) {
            const unsigned char *p = lines.data + i * MAPLINEDEF_SIZE;
            int v1 = (unsigned short)rd_short(p);
            int v2 = (unsigned short)rd_short(p + 2);
            int s0 = rd_short(p + 12);
            int s1 = rd_short(p + 14);

            if (v1 >= nverts || v2 >= nverts) bad_v++;
            if (s0 >= nsides || s1 >= nsides) bad_s++;
        }

        {
            const unsigned char *p = lines.data;
            ps3_logf("[map] linedef[0]: v1=%d v2=%d flags=0x%08x special=%d tag=%d sides=(%d,%d)",
                     (unsigned short)rd_short(p), (unsigned short)rd_short(p + 2),
                     (unsigned)rd_long(p + 4),
                     rd_short(p + 8), rd_short(p + 10),
                     rd_short(p + 12), rd_short(p + 14));
        }

        if (bad_v == 0 && bad_s == 0) {
            ps3_logf("[map]   OK: las %d lineas referencian vertices y sidedefs validos",
                     nlines);
        } else {
            ps3_logf("[map]   *** %d lineas con vertice invalido, %d con sidedef invalido ***",
                     bad_v, bad_s);
        }
    }

    /* --- subsectores: cada uno tiene que apuntar a segs validos --- */
    if (nssecs > 0 && nsegs > 0) {
        int bad = 0;
        for (i = 0; i < nssecs; i++) {
            const unsigned char *p = ssecs.data + i * MAPSUBSECTOR_SIZE;
            int cnt   = (unsigned short)rd_short(p);
            int first = (unsigned short)rd_short(p + 2);
            if (first + cnt > nsegs) bad++;
        }
        if (bad == 0)
            ps3_logf("[map]   OK: los %d subsectores apuntan a segs validos", nssecs);
        else
            ps3_logf("[map]   *** %d subsectores fuera de rango ***", bad);
    }

    /* --- nodos del BSP: cada hijo es o un subsector (bit NF_SUBSECTOR) o
           otro nodo. Si todos los indices caen en rango, el arbol esta
           bien leido, que es lo que despues usa el renderer. --- */
    if (nnodes > 0) {
        int bad = 0;
        for (i = 0; i < nnodes; i++) {
            const unsigned char *p = nodes.data + i * MAPNODE_SIZE;
            int c;
            for (c = 0; c < 2; c++) {
                unsigned int child = (unsigned short)rd_short(p + 24 + c * 2);
                if (child & 0x8000) {               /* NF_SUBSECTOR */
                    if ((int)(child & 0x7fff) >= nssecs) bad++;
                } else {
                    if ((int)child >= nnodes) bad++;
                }
            }
        }
        if (bad == 0)
            ps3_logf("[map]   OK: el arbol BSP de %d nodos cierra bien", nnodes);
        else
            ps3_logf("[map]   *** %d hijos de nodo fuera de rango ***", bad);
    }

    ps3_logf("[map] --- mapa %02d leido ---", mapnum);

    PS3_Wad_Free(&w);
    return 0;
}
