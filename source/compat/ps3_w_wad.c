/* ps3_w_wad.c -- la parte de w_wad.c que hace falta para cargar un mapa.
 *
 * POR QUE NO SE COMPILA w_wad.c DEL MOTOR (todavia):
 * Ese archivo tiene 95 referencias al PVR porque carga texturas directo a la
 * VRAM del Dreamcast, arma la pantalla de carga y maneja los normal maps. Nada
 * de eso hace falta para leer la geometria de un mapa, y arrastrarlo ahora
 * significaria depender del backend RSX que todavia no existe.
 *
 * Asi que aca estan solo las cuatro funciones que p_setup.c consume:
 *   W_OpenMapWad / W_FreeMapLump / W_MapLumpLength / W_GetMapLump
 * con la misma semantica que el original, mas los swaps de endianness.
 *
 * DONDE VAN LOS SWAPS (esto es lo que doom64-dc vacio):
 *   w_wad.c:1478-1479   mapnumlumps = (((bumpwadinfo_t*)mapfileptr)->numlumps);
 *                       infotableofs = (((bumpwadinfo_t*)mapfileptr)->infotableofs);
 *   w_wad.c:1514,1533   maplump[lump].size / .filepos
 *
 * Los parentesis sobrantes en esas lineas son el resto de los LONGSWAP() que
 * DOOM64-RE tenia y que en Dreamcast (little-endian) no hacian falta.
 *
 * La tabla de lumps se convierte UNA VEZ al cargar, asi W_GetMapLump y
 * W_MapLumpLength quedan identicas al original y el resto del motor ve
 * valores nativos sin enterarse de nada.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "kos.h"
#include "../ps3_log.h"

/* --- del motor --- */
/* I_Error de ps3_platform_stub.c. Se declara a mano porque este archivo no
 * incluye doomdef.h (que la define como macro hacia __I_Error). */
void I_Error(char *error, ...) __attribute__((noreturn));

typedef struct memzone_s memzone_t;
extern memzone_t *mainzone;
void *Z_Malloc2(memzone_t *mz, int size, int tag, void *ptr);
void  Z_Free2(memzone_t *mz, void *ptr);
void  Z_Touch(void *ptr);

#define PU_STATIC 1

/* Igual que lumpinfo_t del motor (doomdef.h): int filepos, int size, char[8]. */
typedef struct {
    int  filepos;
    int  size;
    char name[8];
} ps3_lumpinfo_t;

#define LUMPINFO_DISK_SIZE  16

/* --- estado, equivalente a los globals de w_wad.c --- */
char *fnpre = "/dev_hdd0/game/DOOM64CEL/USRDIR";   /* w_wad.c:35 */

static uint8_t        *mapfileptr  = NULL;
static ps3_lumpinfo_t *maplump     = NULL;   /* copia ya convertida */
static int             mapnumlumps = 0;
static char            mapname[8];

/* Lectura little-endian byte a byte: ver ps3_wad.h por que se hace asi. */
static int rd_long(const unsigned char *p)
{
    return (int)((unsigned int)p[0]
               | ((unsigned int)p[1] << 8)
               | ((unsigned int)p[2] << 16)
               | ((unsigned int)p[3] << 24));
}

void PS3_WWad_SetBaseDir(const char *dir)
{
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s", dir);
    fnpre = buf;
    PS3_FS_SetBase(dir);        /* los /pc/... del motor apuntan aca */
}

void W_OpenMapWad(int mapnum)
{
    char    path[320];
    file_t  fd;
    size_t  mapsize;
    int     infotableofs;
    int     i;

    if (mapnum == 0) {
        ps3_log("[wwad] pidieron el mapa 0; se usa el 1");
        mapnum = 1;
    }

    snprintf(mapname, sizeof(mapname), "map%02d", mapnum);
    snprintf(path, sizeof(path), "%s/maps/%s.wad", fnpre, mapname);

    fd = fs_open(path, O_RDONLY);
    if (fd < 0) {
        ps3_logf("[wwad] no se pudo abrir %s", path);
        I_Error("Could not open %s for reading.", path);
        return;
    }

    mapsize = (size_t)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    mapfileptr = (uint8_t *)Z_Malloc2(mainzone, (int)mapsize, PU_STATIC, NULL);
    if (!mapfileptr) {
        fs_close(fd);
        I_Error("sin memoria para %s (%d bytes)", path, (int)mapsize);
        return;
    }

    if ((size_t)read(fd, mapfileptr, mapsize) != mapsize) {
        fs_close(fd);
        I_Error("lectura incompleta de %s", path);
        return;
    }
    fs_close(fd);

    /* Header: aca iban los LONGSWAP. */
    mapnumlumps  = rd_long(mapfileptr + 4);
    infotableofs = rd_long(mapfileptr + 8);

    if (mapnumlumps <= 0 ||
        (size_t)infotableofs + (size_t)mapnumlumps * LUMPINFO_DISK_SIZE > mapsize) {
        ps3_logf("[wwad] tabla fuera de rango: numlumps=%d infotableofs=%d size=%u",
                 mapnumlumps, infotableofs, (unsigned)mapsize);
        I_Error("mapa %s corrupto", mapname);
        return;
    }

    /* Tabla de lumps: el original la apunta directo al buffer. Aca se copia
     * convertida, para que W_GetMapLump/W_MapLumpLength queden iguales al
     * original y todo el resto del motor vea valores nativos. */
    maplump = (ps3_lumpinfo_t *)Z_Malloc2(mainzone,
                  mapnumlumps * (int)sizeof(ps3_lumpinfo_t), PU_STATIC, NULL);
    if (!maplump) {
        I_Error("sin memoria para la tabla de lumps de %s", mapname);
        return;
    }

    for (i = 0; i < mapnumlumps; i++) {
        const unsigned char *p = mapfileptr + infotableofs + (size_t)i * LUMPINFO_DISK_SIZE;
        maplump[i].filepos = rd_long(p);
        maplump[i].size    = rd_long(p + 4);
        memcpy(maplump[i].name, p + 8, 8);
    }

    ps3_logf("[wwad] %s abierto: %d lumps, %u bytes",
             mapname, mapnumlumps, (unsigned)mapsize);
}

void W_FreeMapLump(void)
{
    if (maplump)    { Z_Free2(mainzone, maplump);    maplump = NULL; }
    if (mapfileptr) { Z_Free2(mainzone, mapfileptr); mapfileptr = NULL; }
    mapnumlumps = 0;
}

int W_MapLumpLength(int lump)
{
    if (lump < 0 || lump >= mapnumlumps)
        I_Error("%i out of range", lump);
    return maplump[lump].size;
}

void *W_GetMapLump(int lump)
{
    if (lump < 0 || lump >= mapnumlumps)
        I_Error("lump %d out of range", lump);
    return (void *)(mapfileptr + maplump[lump].filepos);
}

int PS3_WWad_NumLumps(void) { return mapnumlumps; }

/* ==================================================================
 * W_Init y la familia de acceso a lumps.
 *
 * Esta es la parte de w_wad.c que maneja DATOS. Lo que queda afuera es lo
 * que sube texturas a la VRAM del PVR, arma la pantalla de carga y maneja
 * los normal maps: todo eso entra con el backend RSX.
 *
 * Diferencias con el original, y por que:
 *
 *  - Busqueda por nombre LINEAL en vez de la hashtable del motor. Son 1495
 *    lumps y las busquedas ocurren solo al inicializar; la hashtable se puede
 *    poner despues si molesta. Simple y correcto primero.
 *
 *  - Los WAD viven en memoria del sistema (malloc), no en el zone del motor.
 *    Son 4,8 MB entre los dos y el zone tiene 5,4 MB en total; meterlos ahi
 *    no dejaria lugar para el nivel. La PS3 tiene ~200 MB, asi que sobra.
 *
 *  - La tabla de lumps se convierte de endianness UNA vez al cargar, igual
 *    que en W_OpenMapWad.
 * ================================================================== */

#include "dc/pvr.h"

char fnbuf[256];

/* Headers que el renderer usa para los sprites que no son enemigos (items,
 * decoracion, el arma) y para el fogonazo del arma. En el motor son
 * pvr_poly_hdr_t (a pesar del nombre): r_phase3.c y f_main.c los declaran
 * asi. Los compila W_Init, igual que el original (w_wad.c:940-990). */
pvr_poly_hdr_t pvr_sprite_hdr;
pvr_poly_hdr_t pvr_sprite_hdr_bump;
pvr_poly_hdr_t pvr_sprite_hdr_nofilter;
pvr_poly_hdr_t pvr_sprite_hdr_nofilter_bump;
pvr_poly_hdr_t wepnbump_hdr;
pvr_poly_hdr_t wepndecs_hdr;
pvr_poly_hdr_t wepndecs_hdr_nofilter;

/* Todos los sprites que no son enemigos vienen en UNA hoja de 1024x1024 a
 * 8bpp (tex/non_enemy.tex), y el fogonazo en tex/wepn_decs.raw (64x64). */
pvr_ptr_t pvr_non_enemy = NULL;
pvr_ptr_t wepndecs_txr  = NULL;
pvr_ptr_t wepnbump_txr  = NULL;

/* doomdef.h: D64_TPAL(n) y el orden de d64_palette_t (PAL_ENEMY, PAL_ITEM,
 * PAL_FLAT, PAL_I8). Se repiten porque este archivo no incluye doomdef.h. */
#define PS3_PAL_ITEM  1
#define PS3_D64_TPAL(n) (PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL((n)) | PVR_TXRFMT_TWIDDLED)

int   extra_episodes = 0;
int   kneedeep_only  = 0;

/* Las tres paletas de 256 colores del juego (D64MONSTER, D64NONENEMY,
 * PALTEXCONV). En el original las define w_wad.c al incluir este header.
 *
 * La primera version de este archivo tenia  void *D64NONENEMY = NULL;  para
 * que linkeara. Pero r_phase2.c lo usa como int[256][3] para convertir los
 * cielos de nubes: el primer mapa con ese cielo hubiera leido de NULL. */
#include "palettes.h"

void R_InitSymbols(void);

/* --- del motor --- */
void DecodeD64(unsigned char *input, unsigned char *output);
void DecodeJaguar(unsigned char *input, unsigned char *output);

/* doomdef.h: typedef enum { dec_none, dec_jag, dec_d64 } decodetype;
 * Se repite aca porque este archivo no incluye doomdef.h. */
typedef enum { dec_none, dec_jag, dec_d64 } decodetype;

typedef struct { void *cache; } ps3_lumpcache_t;

#define PU_CACHE   16

/* --- IWAD principal (pow2.wad) --- */
static uint8_t         *fullwad     = NULL;
static ps3_lumpinfo_t  *lumpinfo    = NULL;
static ps3_lumpcache_t *lumpcache   = NULL;
static int              numlumps    = 0;

/* --- PWAD de sprites alternativos (alt.wad) --- */
static uint8_t         *s2_fullwad   = NULL;
static ps3_lumpinfo_t  *s2_lumpinfo  = NULL;
static ps3_lumpcache_t *s2_lumpcache = NULL;
static int              s2_numlumps  = 0;

/* Buffer de descompresion. El original usa uno de 256 KB por la misma razon:
 * evitar un Z_Alloc por lump en el camino caliente. */
static uint8_t ps3_decomp_buf[262144] __attribute__((aligned(32)));

/* ------------------------------------------------------------------ */

/* Carga un WAD entero y arma su tabla de lumps ya convertida. */
static int load_wad(const char *path, uint8_t **out_data,
                    ps3_lumpinfo_t **out_lumps, ps3_lumpcache_t **out_cache,
                    int *out_num, const char *label)
{
    file_t fd;
    size_t size;
    int    n, ofs, i;

    fd = fs_open(path, O_RDONLY);
    if (fd < 0) {
        ps3_logf("[wwad] no se pudo abrir %s", path);
        return -1;
    }

    size = (size_t)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    *out_data = (uint8_t *)malloc(size);
    if (!*out_data) {
        ps3_logf("[wwad] sin memoria para %s (%u bytes)", path, (unsigned)size);
        fs_close(fd);
        return -1;
    }

    /* Por bloques: si alguna vez se corta, el log dice donde. */
    {
        size_t rem = size, off = 0;
        while (rem > 0) {
            size_t chunk = rem > (128 * 1024) ? (128 * 1024) : rem;
            ssize_t got = read(fd, *out_data + off, chunk);
            if (got <= 0) {
                ps3_logf("[wwad] lectura cortada en %u/%u de %s",
                         (unsigned)off, (unsigned)size, path);
                fs_close(fd);
                return -1;
            }
            off += (size_t)got;
            rem -= (size_t)got;
        }
    }
    fs_close(fd);

    if (memcmp(*out_data, "IWAD", 4) != 0 && memcmp(*out_data, "PWAD", 4) != 0) {
        ps3_logf("[wwad] %s: identificacion invalida", path);
        return -1;
    }

    n   = rd_long(*out_data + 4);    /* LONGSWAP */
    ofs = rd_long(*out_data + 8);    /* LONGSWAP */

    if (n <= 0 || (size_t)ofs + (size_t)n * LUMPINFO_DISK_SIZE > size) {
        ps3_logf("[wwad] %s: tabla fuera de rango (numlumps=%d ofs=%d size=%u)",
                 path, n, ofs, (unsigned)size);
        return -1;
    }

    *out_lumps = (ps3_lumpinfo_t *)malloc((size_t)n * sizeof(ps3_lumpinfo_t));
    *out_cache = (ps3_lumpcache_t *)malloc((size_t)n * sizeof(ps3_lumpcache_t));
    if (!*out_lumps || !*out_cache) {
        ps3_logf("[wwad] sin memoria para la tabla de %s", path);
        return -1;
    }
    memset(*out_cache, 0, (size_t)n * sizeof(ps3_lumpcache_t));

    for (i = 0; i < n; i++) {
        const unsigned char *p = *out_data + ofs + (size_t)i * LUMPINFO_DISK_SIZE;
        (*out_lumps)[i].filepos = rd_long(p);
        (*out_lumps)[i].size    = rd_long(p + 4);
        memcpy((*out_lumps)[i].name, p + 8, 8);
    }

    *out_num = n;
    ps3_logf("[wwad] %-10s %d lumps, %u KB", label, n, (unsigned)(size / 1024));
    return 0;
}

/* Las 4 paletas del PVR, igual que w_wad.c:598-615:
 *   banco 0 (PAL_ENEMY)  D64MONSTER
 *   banco 1 (PAL_ITEM)   D64NONENEMY
 *   banco 2 (PAL_FLAT)   PALTEXCONV
 *   banco 3 (PAL_I8)     gris
 * El color 0 de cada banco es transparente. */
#define PS3_ARGB1555(r, g, b, a) \
    ((uint16_t)(((!!(a)) << 15) | ((((r) >> 3) & 0x1f) << 10) | \
                ((((g) >> 3) & 0x1f) << 5) | (((b) >> 3) & 0x1f)))

static void setup_palettes(void)
{
    int i;
    pvr_set_pal_format(PVR_PAL_ARGB1555);
    pvr_set_pal_entry(0, 0);
    pvr_set_pal_entry(256, 0);
    pvr_set_pal_entry(512, 0);
    pvr_set_pal_entry(768, 0);
    for (i = 1; i < 256; i++) {
        pvr_set_pal_entry(i,       PS3_ARGB1555(D64MONSTER[i][0], D64MONSTER[i][1], D64MONSTER[i][2], 1));
        pvr_set_pal_entry(256 + i, PS3_ARGB1555(D64NONENEMY[i][0], D64NONENEMY[i][1], D64NONENEMY[i][2], 1));
        pvr_set_pal_entry(512 + i, PS3_ARGB1555(PALTEXCONV[i][0], PALTEXCONV[i][1], PALTEXCONV[i][2], 1));
        pvr_set_pal_entry(768 + i, PS3_ARGB1555(i, i, i, 1));
    }
}

static void load_sprite_sheets(void)
{
    char path[320];
    void *buf = NULL;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/tex/non_enemy.tex", fnpre);
    n = fs_load(path, &buf);
    if (n > 0) {
        pvr_non_enemy = pvr_mem_malloc((size_t)n);
        pvr_txr_load(buf, pvr_non_enemy, (uint32_t)n);
        free(buf);
        ps3_logf("[wwad] tex/non_enemy.tex: %d KB (hoja de sprites)", (int)(n / 1024));
    } else {
        ps3_log("[wwad] tex/non_enemy.tex no cargo: los items no van a tener textura");
    }

    snprintf(path, sizeof(path), "%s/tex/wepn_decs.raw", fnpre);
    n = fs_load(path, &buf);
    if (n > 0) {
        wepndecs_txr = pvr_mem_malloc(64 * 64);
        pvr_txr_load_ex(buf, wepndecs_txr, 64, 64, PVR_TXRLOAD_8BPP);
        free(buf);
    } else {
        ps3_log("[wwad] tex/wepn_decs.raw no cargo: sin textura de fogonazo");
    }
}

static void compile_sprite_headers(void)
{
    pvr_poly_cxt_t cxt;
    int f;

    /* Sprites sin bump, con y sin filtro */
    for (f = 0; f < 2; f++) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PS3_D64_TPAL(PS3_PAL_ITEM), 1024, 1024,
                         pvr_non_enemy, f ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR);
        cxt.gen.specular  = PVR_SPECULAR_ENABLE;
        cxt.gen.fog_type  = PVR_FOG_TABLE;
        cxt.gen.fog_type2 = PVR_FOG_TABLE;
        pvr_poly_compile(f ? &pvr_sprite_hdr_nofilter : &pvr_sprite_hdr, &cxt);
    }

    /* Variantes para cuando el arma tiene bump map (multiplican) */
    for (f = 0; f < 2; f++) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PS3_D64_TPAL(PS3_PAL_ITEM), 1024, 1024,
                         pvr_non_enemy, f ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR);
        cxt.gen.specular   = PVR_SPECULAR_ENABLE;
        cxt.gen.fog_type   = PVR_FOG_TABLE;
        cxt.gen.fog_type2  = PVR_FOG_TABLE;
        cxt.blend.src      = PVR_BLEND_DESTCOLOR;
        cxt.blend.dst      = PVR_BLEND_ZERO;
        cxt.blend.src_enable = 0;
        cxt.blend.dst_enable = 1;
        pvr_poly_compile(f ? &pvr_sprite_hdr_nofilter_bump : &pvr_sprite_hdr_bump, &cxt);
    }

    /* Fogonazo del arma */
    for (f = 0; f < 2; f++) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PS3_D64_TPAL(PS3_PAL_ITEM), 64, 64,
                         wepndecs_txr, f ? PVR_FILTER_NONE : PVR_FILTER_BILINEAR);
        cxt.gen.specular  = PVR_SPECULAR_ENABLE;
        cxt.gen.fog_type  = PVR_FOG_TABLE;
        cxt.gen.fog_type2 = PVR_FOG_TABLE;
        pvr_poly_compile(f ? &wepndecs_hdr_nofilter : &wepndecs_hdr, &cxt);
    }

    /* Bump del arma: formato BUMP, el backend lo saltea mientras no haya
     * normal maps. Se compila igual para que el header sea valido. */
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_BUMP | PVR_TXRFMT_TWIDDLED,
                     64, 64, wepnbump_txr, PVR_FILTER_BILINEAR);
    cxt.gen.specular = PVR_SPECULAR_ENABLE;
    cxt.txr.env      = PVR_TXRENV_DECAL;
    cxt.blend.src    = PVR_BLEND_ONE;
    cxt.blend.dst    = PVR_BLEND_ZERO;
    cxt.blend.dst_enable = 1;
    pvr_poly_compile(&wepnbump_hdr, &cxt);
}

/* ------------------------------------------------------------------ */
/* Episodios extra (w_wad.c:620-665 del original)                        */
/*                                                                      */
/*  - Lost Levels: maps/map34..40.wad, sacados del DOOM64.WAD del         */
/*    remaster (ps3_romconv.c). Se validan por tamano + MD5, igual que el */
/*    original: si alguno no coincide, no se habilitan.                   */
/*  - Knee Deep in the Dead (contenido extra de doom64-dc): maps/map41.   */
/* extra_episodes: 0 = ninguno, 1 = uno de los dos, 2 = los dos.          */
/* ------------------------------------------------------------------ */
#include "md5.h"

static const uint8_t s_md5_lost[7][16] = {
    {0xc0,0xb6,0x50,0x82,0x8e,0x55,0x2e,0x4c,0x75,0xa9,0x3c,0xcb,0x39,0x4c,0x89,0x75},
    {0x11,0x3e,0x8c,0x80,0x46,0x9b,0x53,0xd6,0xab,0x92,0xcd,0x47,0x71,0x53,0x52,0x0a},
    {0x54,0x59,0xda,0x76,0xa3,0xdf,0x1d,0xfa,0x0a,0x32,0x60,0x02,0xe4,0xe9,0x04,0xbe},
    {0x62,0x34,0xa3,0xad,0x54,0x2f,0x44,0x41,0xc9,0xf9,0x1e,0xab,0x77,0x42,0xaf,0xc6},
    {0x60,0x5b,0x30,0x72,0x6b,0x9b,0x0d,0xb6,0x8d,0x8e,0x62,0x6b,0x42,0x33,0x7a,0xe9},
    {0x63,0xf6,0x9f,0x1b,0x9a,0xdf,0xf7,0xb8,0x94,0x2b,0x70,0x4d,0x89,0x41,0x32,0xec},
    {0xf4,0x2b,0x74,0xf5,0xa5,0xa6,0xa7,0xa4,0x62,0xc0,0xcf,0xdb,0xfe,0x4a,0xd5,0xe7},
};
static const long s_size_lost[7] = { 253568, 360456, 311768, 395260, 361192, 215276, 61996 };

static void detect_episodes(void)
{
    static unsigned char buf[65536];
    char path[320];
    int i, lost = 1;

    extra_episodes = 0;
    kneedeep_only = 0;

    /* OJO: despues de R_InitSymbols fnpre queda en "/pc"; fs_open lo traduce
     * a la carpeta del juego (fopen directo no). */
    for (i = 34; i <= 40 && lost; i++) {
        int fd;
        MD5_CTX ctx;
        unsigned char sum[16];
        long size, left;

        snprintf(path, sizeof(path), "%s/maps/map%d.wad", fnpre, i);
        fd = fs_open(path, O_RDONLY);
        if (fd < 0) {
            if (i == 34) ps3_log("[wwad] Lost Levels: no estan (maps/map34..40)");
            else ps3_logf("[wwad] Lost Levels: falta map%d", i);
            lost = 0;
            break;
        }
        size = (long)lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (size != s_size_lost[i - 34]) {
            ps3_logf("[wwad] Lost Levels: map%d mide %ld, se esperaba %ld", i, size, s_size_lost[i - 34]);
            fs_close(fd);
            lost = 0;
            break;
        }
        MD5Init(&ctx);
        for (left = size; left > 0; ) {
            size_t n = (size_t)(left > (long)sizeof(buf) ? (long)sizeof(buf) : left);
            if (fs_read(fd, buf, n) != (ssize_t)n) { left = -1; break; }
            MD5Update(&ctx, buf, (unsigned int)n);
            left -= (long)n;
        }
        fs_close(fd);
        MD5Final(sum, &ctx);
        if (left != 0 || memcmp(sum, s_md5_lost[i - 34], 16)) {
            ps3_logf("[wwad] Lost Levels: map%d no coincide con el MD5 esperado", i);
            lost = 0;
            break;
        }
    }
    if (lost) {
        extra_episodes++;
        ps3_log("[wwad] Lost Levels: OK (7 mapas verificados)");
    }

    /* Knee Deep: los mapas y el cielo (doom1mn.lmp; sin el, r_phase2.c
     * corta con I_Error al entrar al primer mapa). */
    {
        int fd, ok = 1;
        snprintf(path, sizeof(path), "%s/maps/map41.wad", fnpre);
        fd = fs_open(path, O_RDONLY);
        if (fd >= 0) fs_close(fd); else ok = 0;
        if (ok) {
            snprintf(path, sizeof(path), "%s/doom1mn.lmp", fnpre);
            fd = fs_open(path, O_RDONLY);
            if (fd >= 0) fs_close(fd);
            else { ok = 0; ps3_log("[wwad] Knee Deep: estan los mapas pero falta doom1mn.lmp"); }
        }
        path[0] = ok ? 1 : 0;
    }
    if (path[0]) {
        if (extra_episodes == 0)
            kneedeep_only = 1;
        extra_episodes++;
        ps3_log("[wwad] Knee Deep in the Dead (map41..): presente");
    }
    ps3_logf("[wwad] episodios extra: %d%s", extra_episodes, kneedeep_only ? " (solo Knee Deep)" : "");
}

void W_Init(void)
{
    char path[320];

    ps3_log("[wwad] --- W_Init ---");

    snprintf(path, sizeof(path), "%s/pow2.wad", fnpre);
    if (load_wad(path, &fullwad, &lumpinfo, &lumpcache, &numlumps, "pow2.wad") < 0)
        I_Error("no se pudo cargar pow2.wad");

    snprintf(path, sizeof(path), "%s/alt.wad", fnpre);
    if (load_wad(path, &s2_fullwad, &s2_lumpinfo, &s2_lumpcache,
                 &s2_numlumps, "alt.wad") < 0)
        ps3_log("[wwad] alt.wad no cargo: los sprites alternativos no van a estar");

    setup_palettes();

    /* HUD: los numeros y simbolos (symbols.raw). Lo llama el W_Init
     * original; abre "/pc/symbols.raw", que ps3_fs_open traduce. */
    ps3_log("[wwad] R_InitSymbols (HUD)");
    R_InitSymbols();

    load_sprite_sheets();
    compile_sprite_headers();

    detect_episodes();

    ps3_log("[wwad] --- W_Init OK ---");
}

/* ------------------------------------------------------------------ */
/* Busqueda por nombre                                                 */
/* ------------------------------------------------------------------ */

/* Compara un nombre de lump (8 bytes, sin NUL obligatorio) contra una cadena.
 * El bit alto del primer caracter marca "comprimido" y no es parte del nombre. */
static int name_matches(const char *lumpname, const char *want)
{
    int i;
    for (i = 0; i < 8; i++) {
        unsigned char a = (unsigned char)lumpname[i];
        unsigned char b = (unsigned char)want[i];
        if (i == 0) a &= 0x7f;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
        if (b == '\0') return 1;
    }
    return 1;
}

static int find_lump(const ps3_lumpinfo_t *lumps, int n, const char *name)
{
    char want[9];
    int  i, len;

    len = (int)strlen(name);
    if (len > 8) len = 8;
    memset(want, 0, sizeof(want));
    memcpy(want, name, (size_t)len);

    for (i = n - 1; i >= 0; i--)          /* de atras para adelante, como Doom */
        if (name_matches(lumps[i].name, want))
            return i;
    return -1;
}

int W_CheckNumForName(char *name)
{
    return find_lump(lumpinfo, numlumps, name);
}

int W_GetNumForName(char *name)
{
    int i = W_CheckNumForName(name);
    if (i == -1)
        I_Error("W_GetNumForName: %s no encontrado", name);
    return i;
}

char *W_GetNameForNum(int num)
{
    static char retname[9];

    if (num < 0 || num >= numlumps)
        I_Error("W_GetNameForNum: %d fuera de rango", num);

    memcpy(retname, lumpinfo[num].name, 8);
    retname[0] &= 0x7f;                    /* sacar la marca de comprimido */
    retname[8] = '\0';
    return retname;
}

int W_LumpLength(int lump)
{
    if (lump < 0 || lump >= numlumps)
        I_Error("W_LumpLength: lump %i fuera de rango", lump);
    return lumpinfo[lump].size;
}

/* ------------------------------------------------------------------ */
/* Lectura y cache                                                     */
/* ------------------------------------------------------------------ */

void W_ReadLump(int lump, void *dest, decodetype dectype)
{
    ps3_lumpinfo_t *l;

    if (lump < 0 || lump >= numlumps)
        I_Error("W_ReadLump: lump %i fuera de rango", lump);

    l = &lumpinfo[lump];

    /* El bit alto del primer caracter del nombre marca lump comprimido. */
    if (l->name[0] & 0x80) {
        /* El tamano comprimido es la distancia al lump SIGUIENTE, porque el
         * campo size guarda el tamano YA DESCOMPRIMIDO. */
        int comp_size = (lump + 1 < numlumps)
                      ? (l[1].filepos - l->filepos)
                      : (l->size);

        if (comp_size < 0 || comp_size > (int)sizeof(ps3_decomp_buf))
            I_Error("W_ReadLump: lump %i comprimido mide %d", lump, comp_size);

        memcpy(ps3_decomp_buf, fullwad + l->filepos, (size_t)comp_size);

        if (dectype == dec_jag)
            DecodeJaguar(ps3_decomp_buf, (uint8_t *)dest);
        else
            DecodeD64(ps3_decomp_buf, (uint8_t *)dest);
    } else {
        memcpy(dest, fullwad + l->filepos, (size_t)l->size);
    }
}

/* OJO con el cuarto argumento de Z_Malloc2: NO es "de donde sacar memoria",
 * es el USER POINTER del allocator de Doom. Z_Malloc2 escribe el puntero
 * resultante en *user, y cuando despues purga el bloque (los PU_CACHE se
 * purgan solos cuando hace falta lugar) le vuelve a escribir NULL.
 *
 * O sea: el campo del lumpcache tiene que ser EL user pointer. Pasando NULL
 * el bloque se reserva igual y todo parece andar, pero al primer purgue el
 * lumpcache queda apuntando a memoria liberada y el crash aparece mucho
 * despues, en otro lado y sin relacion aparente. Es el mismo patron que usa
 * el w_wad.c original (Z_Malloc(size, tag, &lc->cache)). */
void *W_CacheLumpNum(int lump, int tag, decodetype dectype)
{
    if (lump < 0 || lump >= numlumps)
        I_Error("W_CacheLumpNum: lump %i fuera de rango", lump);

    if (!lumpcache[lump].cache) {
        Z_Malloc2(mainzone, lumpinfo[lump].size, tag, &lumpcache[lump].cache);
        if (!lumpcache[lump].cache)
            I_Error("W_CacheLumpNum: sin memoria para el lump %i (%d bytes)",
                    lump, lumpinfo[lump].size);
        W_ReadLump(lump, lumpcache[lump].cache, dectype);
    } else if (tag & PU_CACHE) {
        /* Rejuvenecer el bloque, si no el aging lo purga aunque se use. */
        Z_Touch(lumpcache[lump].cache);
    }
    return lumpcache[lump].cache;
}

void *W_CacheLumpName(char *name, int tag, decodetype dectype)
{
    return W_CacheLumpNum(W_GetNumForName(name), tag, dectype);
}

/* ------------------------------------------------------------------ */
/* alt.wad (sprites alternativos)                                      */
/* ------------------------------------------------------------------ */

int W_S2_CheckNumForName(char *name)
{
    return find_lump(s2_lumpinfo, s2_numlumps, name);
}

int W_S2_GetNumForName(char *name)
{
    int i = W_S2_CheckNumForName(name);
    if (i == -1)
        I_Error("W_S2_GetNumForName: %s no encontrado", name);
    return i;
}

int W_S2_LumpLength(int lump)
{
    if (lump < 0 || lump >= s2_numlumps)
        I_Error("W_S2_LumpLength: lump %i fuera de rango", lump);
    return s2_lumpinfo[lump].size;
}

/* OJO: en alt.wad TODOS los lumps vienen comprimidos con el formato de
 * Jaguar (lo hace asi el wadtool), sin importar el bit alto del nombre. El
 * original (w_wad.c:1253) siempre llama a DecodeJaguar. La primera version de
 * este archivo miraba el bit y usaba DecodeD64: el decodificador equivocado
 * escribe basura MAS ALLA del buffer y pisa el header del bloque siguiente
 * del zone. Sintoma: "freed a pointer without ZONEID" en el primer frame que
 * dibuja un enemigo. */
void W_S2_ReadLump(int lump, void *dest)
{
    ps3_lumpinfo_t *l;
    int comp_size;

    if (lump < 0 || lump >= s2_numlumps)
        I_Error("W_S2_ReadLump: lump %i fuera de rango", lump);

    l = &s2_lumpinfo[lump];
    comp_size = (lump + 1 < s2_numlumps) ? (l[1].filepos - l->filepos)
                                         : (l->size);
    if (comp_size < 0 || comp_size > (int)sizeof(ps3_decomp_buf))
        I_Error("W_S2_ReadLump: lump %i comprimido mide %d", lump, comp_size);

    memcpy(ps3_decomp_buf, s2_fullwad + l->filepos, (size_t)comp_size);
    DecodeJaguar(ps3_decomp_buf, (uint8_t *)dest);
}

void *W_S2_CacheLumpNum(int lump, int tag)
{
    if (lump < 0 || lump >= s2_numlumps)
        I_Error("W_S2_CacheLumpNum: lump %i fuera de rango", lump);

    if (!s2_lumpcache[lump].cache) {
        Z_Malloc2(mainzone, s2_lumpinfo[lump].size, tag, &s2_lumpcache[lump].cache);
        if (!s2_lumpcache[lump].cache)
            I_Error("W_S2_CacheLumpNum: sin memoria para el lump %i", lump);
        W_S2_ReadLump(lump, s2_lumpcache[lump].cache);
    } else if (tag & PU_CACHE) {
        Z_Touch(s2_lumpcache[lump].cache);
    }
    return s2_lumpcache[lump].cache;
}

void *W_S2_CacheLumpName(char *name, int tag)
{
    return W_S2_CacheLumpNum(W_S2_GetNumForName(name), tag);
}

/* ------------------------------------------------------------------ */
/* bump.wad: normal maps, exclusivos del port de Dreamcast.            */
/* ------------------------------------------------------------------ */

#define WSTUB()                                                  \
    do {                                                         \
        static int _s = 0;                                       \
        if (!_s) { _s = 1;                                       \
            ps3_logf("[stub] %s()  (pendiente: etapa 3)", __func__); } \
    } while (0)

int   W_Bump_CheckNumForName(char *n)       { (void)n; WSTUB(); return -1; }
/* -1 = "esta textura no tiene normal map". NO devolver 0: P_CachePvrTexture
 * lo toma como lump valido y reserva un normal map para cada textura. */
int   W_Bump_GetNumForName(char *n)         { (void)n; WSTUB(); return -1; }
int   W_Bump_LumpLength(int l)              { (void)l; WSTUB(); return 0; }
void  W_Bump_ReadLump(int l, void *d, int w, int h)
{ (void)l; (void)d; (void)w; (void)h; WSTUB(); }
void  W_ReplaceWeaponBumps(int wepn)        { (void)wepn; WSTUB(); }
void  W_ListExtraMaps(void)                 { WSTUB(); }

/* La pantalla de carga necesita el RSX; por ahora no dibuja nada. */
void  W_DrawLoadScreen(char *what, int cur, int tot)
{ (void)what; (void)cur; (void)tot; }

int PS3_WWad_NumLumps_Main(void) { return numlumps; }
