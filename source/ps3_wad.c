/* ps3_wad.c -- lector de WAD para Doom 64. Ver ps3_wad.h por el tema del
 * endianness, que es la razon de ser de este archivo.
 *
 * Esta es la version de validacion (etapa 1a): carga el archivo entero, arma
 * la tabla de lumps ya convertida, y ofrece un chequeo de coherencia. Todavia
 * no cachea lumps ni descomprime nada -- eso viene cuando entre el w_wad.c
 * real del motor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3_wad.h"
#include "ps3_log.h"

int PS3_Wad_Load(wadfile_t *w, const char *path)
{
    FILE *f;
    long  fsize;
    size_t got;
    int i;

    memset(w, 0, sizeof(*w));
    snprintf(w->path, sizeof(w->path), "%s", path);

    f = fopen(path, "rb");
    if (!f) {
        ps3_logf("[wad] NO se pudo abrir %s", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < (long)WADINFO_SIZE) {
        ps3_logf("[wad] %s demasiado chico (%ld bytes)", path, fsize);
        fclose(f);
        return -1;
    }

    w->data = (unsigned char *)malloc((size_t)fsize);
    if (!w->data) {
        ps3_logf("[wad] sin memoria para %s (%ld bytes)", path, fsize);
        fclose(f);
        return -1;
    }

    /* Lectura por bloques: leer 4 MB de una sola fread tambien anda, pero por
     * bloques se ve en el log donde se corta si alguna vez se corta. */
    {
        size_t rem = (size_t)fsize;
        size_t off = 0;
        while (rem > 0) {
            size_t chunk = rem > (128 * 1024) ? (128 * 1024) : rem;
            got = fread(w->data + off, 1, chunk, f);
            if (got == 0) {
                ps3_logf("[wad] lectura cortada en %u/%ld de %s",
                         (unsigned)off, fsize, path);
                fclose(f);
                free(w->data);
                w->data = NULL;
                return -1;
            }
            off += got;
            rem -= got;
        }
    }
    fclose(f);
    w->size = (size_t)fsize;

    /* Header, byte a byte (ver ps3_wad.h). */
    memcpy(w->ident, w->data, 4);
    w->ident[4] = '\0';

    if (memcmp(w->data, "IWAD", 4) != 0 &&
        memcmp(w->data, "PWAD", 4) != 0) {
        ps3_logf("[wad] %s: identificacion invalida '%s'", path, w->ident);
        free(w->data);
        w->data = NULL;
        return -1;
    }

    w->numlumps     = wad_rd_long(w->data + 4);
    w->infotableofs = wad_rd_long(w->data + 8);

    if (w->numlumps <= 0 || w->infotableofs < 0 ||
        (long)w->infotableofs + (long)w->numlumps * LUMPINFO_SIZE > fsize) {
        ps3_logf("[wad] %s: tabla de lumps fuera de rango "
                 "(numlumps=%d infotableofs=%d filesize=%ld)",
                 path, w->numlumps, w->infotableofs, fsize);
        ps3_log ("[wad]   -> casi seguro el endianness esta al reves");
        free(w->data);
        w->data = NULL;
        return -1;
    }

    w->lumps = (lumpinfo_t *)malloc((size_t)w->numlumps * sizeof(lumpinfo_t));
    if (!w->lumps) {
        ps3_logf("[wad] sin memoria para la tabla de lumps de %s", path);
        free(w->data);
        w->data = NULL;
        return -1;
    }

    /* Tabla de lumps, entrada por entrada y campo por campo.
     * DOOM64-RE hace la conversion equivalente en w_wad.c:99-100; doom64-dc
     * borro ese loop entero porque en Dreamcast no hacia falta. */
    for (i = 0; i < w->numlumps; i++) {
        const unsigned char *p = w->data + w->infotableofs + (size_t)i * LUMPINFO_SIZE;

        w->lumps[i].filepos = wad_rd_long(p + LUMPINFO_OFS_POS);
        w->lumps[i].size    = wad_rd_long(p + LUMPINFO_OFS_SIZE);
        memcpy(w->lumps[i].name, p + LUMPINFO_OFS_NAME, 8);
    }

    return 0;
}

void PS3_Wad_Free(wadfile_t *w)
{
    if (w->lumps) { free(w->lumps); w->lumps = NULL; }
    if (w->data)  { free(w->data);  w->data  = NULL; }
    w->numlumps = 0;
    w->size = 0;
}

int PS3_Wad_Validate(const wadfile_t *w)
{
    int bad = 0;
    int i;

    for (i = 0; i < w->numlumps; i++) {
        long fp = (long)WAD_FILEPOS(w->lumps[i].filepos);
        long sz = (long)w->lumps[i].size;

        if (fp < 0 || sz < 0 || fp + sz > (long)w->size)
            bad++;
    }
    return bad;
}

void PS3_Wad_LumpName(const lumpinfo_t *l, char out[9])
{
    int i;
    for (i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)l->name[i];
        if (i == 0) c &= 0x7f;      /* bit alto = marca de comprimido */
        out[i] = (c >= 32 && c < 127) ? (char)c : (c == 0 ? '\0' : '?');
        if (out[i] == '\0') break;
    }
    out[i] = '\0';
    out[8] = '\0';
}

int PS3_Wad_FindLump(const wadfile_t *w, const char *name)
{
    char nm[9];
    int i;

    for (i = 0; i < w->numlumps; i++) {
        PS3_Wad_LumpName(&w->lumps[i], nm);
        if (strcasecmp(nm, name) == 0)
            return i;
    }
    return -1;
}
