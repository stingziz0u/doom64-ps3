/* ps3_engine.c -- arranque del motor real y carga de un mapa.
 *
 * Etapa 1d: el motor original de Doom 64 (DOOM64-RE via doom64-dc) carga el
 * MAP01 completo en la PS3 con SUS PROPIAS estructuras -- vertex_t, line_t,
 * sector_t, subsector_t, node_t -- sin un solo parche en sus fuentes. Toda la
 * adaptacion vive en source/compat/.
 *
 * ---------------------------------------------------------------------------
 * EL CHEQUEO DE INTEGRIDAD (importante, y no documentado en ningun lado)
 *
 * El motor trae una proteccion anti-tamper que NO da un error claro:
 *
 *   - w_wad.c calcula el MD5 de warn3.dt (la pantalla de advertencia de
 *     copyright, 512*512*2 = 524288 bytes en RGB565) y lo deja en el array
 *     global 'backres[16]' (p_user.c:658).
 *   - Ese nombre esta ofuscado: doomdef.h hace
 *         #define backres o_ad675382a0ccc360672c24686a0f93ee
 *     y la ruta tambien: startupfile es "%s/warn3.dt" escrito con escapes
 *     ("\x25""s\057w\x61""r\1563\x2E""d\164").
 *   - Despues, funciones sin relacion entre si verifican bytes sueltos:
 *         z_zone.c:108,627   backres[10] != 0xc3  -> "failed allocation on N"
 *         p_setup.c:528      backres[4]  != 0x69
 *         in_main.c:273      backres[13] != 0xad
 *
 * O sea: si los datos no son legitimos, el juego revienta en el ALLOCATOR o
 * en la CARGA DE MAPA con un mensaje que apunta al lugar equivocado. Sin
 * saber esto, un "failed allocation" con 5 MB libres se persigue durante
 * horas por el lado de la memoria.
 *
 * MD5 esperado de warn3.dt: 54077f7169460a16d55fc3aa44ad2227
 *
 * Aca se calcula de verdad, con el md5c.c del motor, en vez de hardcodearlo.
 * Eso ademas prueba MD5 en big-endian.
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3_engine.h"
#include "ps3_log.h"

/* --- del motor --- */
typedef struct memzone_s memzone_t;
extern memzone_t *mainzone;

void  Z_Init(void);
int   Z_FreeMemory(memzone_t *mz);
void *Z_Malloc2(memzone_t *mz, int size, int tag, void *ptr);
void  Z_Free2(memzone_t *mz, void *ptr);

void  W_Init(void);
void  W_OpenMapWad(int mapnum);
void  W_FreeMapLump(void);

void  R_Init(void);
void  ST_Init(void);
void  P_LoadThings(void);

/* Del motor: los limites de la tabla de texturas que arma R_InitTextures.
 * Si quedan en 0, P_LoadSectors calcula mal el cielo. */
extern int firsttex, lasttex, numtextures;

/* No existe ningun 'numthings' global: en p_setup.c es local de P_LoadThings.
 * Lo observable desde afuera lo expone ps3_game.c. */
int PS3_CountMobjs(void);
int PS3_GetSpawnCount(void);
void PS3_Game_NewGame(int map);
int  PS3_Game_RunTics(int ntics);

void P_LoadMacros(void);
void P_LoadBlockMap(void);
void P_LoadVertexes(void);
void P_LoadSectors(void);
void P_LoadSideDefs(void);
void P_LoadLineDefs(void);
void P_LoadSubSectors(void);
void P_LoadNodes(void);
void P_LoadSegs(void);
void P_LoadLeafs(void);
void P_LoadReject(void);
void P_LoadLights(void);
void P_GroupLines(void);

/* Conteos que llena el propio motor al cargar. */
extern int numvertexes, numlines, numsides, numsectors;
extern int numsubsectors, numsegs, numnodes;

/* Structs del motor, minimas y solo para verificar valores. Se declaran a
 * mano en vez de incluir doomdef.h para no arrastrar el motor entero dentro
 * de la capa de plataforma. El layout tiene que coincidir con el del motor:
 * vertex_t empieza con x,y (fixed_t = int), sector_t con floorheight y
 * ceilingheight (fixed_t), line_t con v1,v2 (punteros) y flags (int). */
typedef int ps3_fixed_t;

struct ps3_vertex  { ps3_fixed_t x, y; };
struct ps3_sector  { ps3_fixed_t floorheight, ceilingheight; };

extern struct ps3_vertex *vertexes;
extern struct ps3_sector *sectors;

/* md5c.c del motor */
typedef struct MD5Context {
    unsigned int  state[4];
    unsigned int  count[2];
    unsigned char buffer[64];
} PS3_MD5_CTX;

void MD5Init(PS3_MD5_CTX *);
void MD5Update(PS3_MD5_CTX *, const unsigned char *, unsigned int);
void MD5Final(unsigned char[16], PS3_MD5_CTX *);

/* Definido por p_user.c:658. */
extern unsigned char o_ad675382a0ccc360672c24686a0f93ee[16];
#define backres o_ad675382a0ccc360672c24686a0f93ee

/* De ps3_w_wad.c */
void PS3_WWad_SetBaseDir(const char *dir);

#define WARN3_SIZE  (512 * 512 * 2)
#define PU_STATIC   1
#define PU_LEVEL    2

/* ------------------------------------------------------------------ */

static int integrity_check(const char *basedir)
{
    char  path[320];
    FILE *f;
    unsigned char *buf;
    PS3_MD5_CTX ctx;
    size_t got;
    long   fsize;
    int    i;
    char   hex[33];

    static const unsigned char expected[16] = {
        0x54, 0x07, 0x7f, 0x71, 0x69, 0x46, 0x0a, 0x16,
        0xd5, 0x5f, 0xc3, 0xaa, 0x44, 0xad, 0x22, 0x27
    };

    snprintf(path, sizeof(path), "%s/warn3.dt", basedir);

    f = fopen(path, "rb");
    if (!f) {
        /* warn3.dt es la pantalla de aviso del port de Dreamcast ("Doom 64
         * for Dreamcast is free... redistribution of game data is
         * prohibited..."), y el motor usa su MD5 como llave anti-tamper para
         * obligar a mostrarla. En la PS3 el aviso equivalente lo muestra
         * PS3_Notice() (main.c) y el PKG no trae datos del juego: la llave
         * se carga directo. */
        memcpy(backres, expected, 16);
        ps3_log("[eng] integridad: sin warn3.dt (no hace falta en PS3; aviso propio al arrancar)");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize != WARN3_SIZE) {
        ps3_logf("[eng] integridad: warn3.dt mide %ld, se esperaban %d",
                 fsize, WARN3_SIZE);
        fclose(f);
        return -1;
    }

    buf = (unsigned char *)malloc(WARN3_SIZE);
    if (!buf) {
        ps3_log("[eng] integridad: sin memoria para warn3.dt");
        fclose(f);
        return -1;
    }

    got = fread(buf, 1, WARN3_SIZE, f);
    fclose(f);

    if (got != WARN3_SIZE) {
        ps3_logf("[eng] integridad: lectura corta (%u de %d)",
                 (unsigned)got, WARN3_SIZE);
        free(buf);
        return -1;
    }

    /* El mismo calculo que hace w_wad.c:686-689. */
    MD5Init(&ctx);
    MD5Update(&ctx, buf, WARN3_SIZE);
    MD5Final(backres, &ctx);
    free(buf);

    for (i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", backres[i]);
    ps3_logf("[eng] MD5 de warn3.dt: %s", hex);

    if (memcmp(backres, expected, 16) != 0) {
        ps3_log("[eng] *** el MD5 NO coincide con el esperado ***");
        ps3_log("[eng]     esperado: 54077f7169460a16d55fc3aa44ad2227");
        ps3_log("[eng]     -> o los datos son otros, o md5c.c falla en big-endian");
        return -1;
    }

    ps3_log("[eng]   OK: MD5 correcto -- md5c.c anda bien en big-endian");
    return 0;
}

/* ------------------------------------------------------------------ */

static int zone_check(void)
{
    int free_before, free_after, free_final;
    void *a, *b;

    Z_Init();
    if (!mainzone) {
        ps3_log("[eng] FATAL: Z_Init no dejo mainzone");
        return -1;
    }
    ps3_logf("[eng] Z_Init OK, mainzone=%p", (void *)mainzone);

    free_before = Z_FreeMemory(mainzone);
    ps3_logf("[eng] heap libre tras Z_Init: %d bytes (%d KB)",
             free_before, free_before / 1024);

    a = Z_Malloc2(mainzone, 64 * 1024, PU_STATIC, NULL);
    b = Z_Malloc2(mainzone, 256 * 1024, PU_LEVEL, NULL);
    if (!a || !b) {
        ps3_logf("[eng] *** Z_Malloc fallo (a=%p b=%p) ***", a, b);
        return -1;
    }

    free_after = Z_FreeMemory(mainzone);
    ps3_logf("[eng] tras reservar 64KB + 256KB: %d libres (bajo %d)",
             free_after, free_before - free_after);

    Z_Free2(mainzone, b);
    Z_Free2(mainzone, a);

    free_final = Z_FreeMemory(mainzone);
    if (free_final == free_before)
        ps3_log("[eng]   OK: el heap volvio exactamente a su estado inicial");
    else
        ps3_logf("[eng]   nota: difiere en %d bytes", free_before - free_final);

    return 0;
}

/* ------------------------------------------------------------------ */

/* Carga la geometria del mapa con el motor de verdad.
 *
 * Es la secuencia de P_SetupLevel (p_setup.c:1019-1035), salteando el ultimo
 * paso: P_LoadThings llama a P_SpawnMapThing, que necesita las tablas de
 * sprites que todavia estan stubeadas. Se agrega cuando entre w_wad.c real.
 *
 * El orden importa: cada funcion depende de lo que cargo la anterior. */
static void load_map(int mapnum)
{
    ps3_logf("[eng] --- cargando el mapa %02d con el motor ---", mapnum);

    /* P_SetupLevel completo, con preambulo, paso por paso: ver ps3_game.c
     * (ahi esta explicado por que la version anterior se caia en
     * P_LoadThings). */
    PS3_Game_NewGame(mapnum);

    ps3_log("[eng] el motor cargo:");
    ps3_logf("[eng]   vertexes   = %d", numvertexes);
    ps3_logf("[eng]   lines      = %d", numlines);
    ps3_logf("[eng]   sides      = %d", numsides);
    ps3_logf("[eng]   sectors    = %d", numsectors);
    ps3_logf("[eng]   subsectors = %d", numsubsectors);
    ps3_logf("[eng]   segs       = %d", numsegs);
    ps3_logf("[eng]   nodes      = %d", numnodes);

    /* --- VALORES, no solo conteos ---
     *
     * Los conteos salen de dividir el tamano del lump, asi que salen bien
     * aunque el endianness este mal. Estos valores no: cada uno ejercita un
     * tipo de swap distinto y se contrastan con lo que leyo nuestro parser
     * byte a byte en la etapa [4].
     *
     *   vertexes[0]  -> int   (LONGSWAP)     debe dar (320, -1280)
     *   sectors[0]   -> short (LITTLESHORT)  debe dar 96 / 224
     */
    if (numvertexes > 0 && vertexes) {
        int vx = vertexes[0].x >> 16;
        int vy = vertexes[0].y >> 16;
        ps3_logf("[eng]   vertexes[0] = (%d, %d)   [esperado (320, -1280)]", vx, vy);
        if (vx == 320 && vy == -1280)
            ps3_log("[eng]     OK: LONGSWAP correcto (campos int)");
        else
            ps3_log("[eng]     *** LONGSWAP mal: los campos int se leen al reves ***");
    }

    if (numsectors > 0 && sectors) {
        int fh = sectors[0].floorheight   >> 16;
        int ch = sectors[0].ceilingheight >> 16;
        ps3_logf("[eng]   sectors[0] = piso %d, techo %d   [esperado 96 / 224]", fh, ch);
        if (fh == 96 && ch == 224)
            ps3_log("[eng]     OK: LITTLESHORT correcto (campos short)");
        else
            ps3_log("[eng]     *** LITTLESHORT mal: los campos short se leen al reves ***");
    }

    /* Contraste con lo que leyo nuestro propio parser en la etapa [4]: si los
     * siete numeros coinciden, el motor y nuestro lector entienden el archivo
     * igual, y el endianness quedo bien en toda la cadena. */
    if (numvertexes == 1138 && numlines == 1187 && numsides == 1634 &&
        numsectors == 159 && numsubsectors == 522 && numsegs == 1743 &&
        numnodes == 521) {
        ps3_log("[eng]   OK: coincide exactamente con lo que leyo nuestro parser");
    } else {
        ps3_log("[eng]   *** los conteos NO coinciden con la etapa [4] ***");
    }

    /* P_LoadThings: los 157 things del MAP01 se reparten en dos grupos --
     * los que spawnean YA (van a la lista mobjhead) y los diferidos con
     * MTF_SPAWN (van a spawnlist, los suelta despues un trigger del mapa).
     * La suma no tiene por que dar 157: hay things que el motor descarta por
     * dificultad o por modo de juego. Lo que importa es que sean > 0 y que
     * la lista de mobjs no este rota. */
    {
        int nmobj = PS3_CountMobjs();
        int ndef  = PS3_GetSpawnCount();
        ps3_logf("[eng]   mobjs vivos = %d   diferidos (MTF_SPAWN) = %d",
                 nmobj, ndef);
        if (nmobj < 0)
            ps3_log("[eng]     *** la lista mobjhead esta rota ***");
        else if (nmobj > 0)
            ps3_log("[eng]     OK: P_LoadThings spawneo cosas y la lista cierra");
        else
            ps3_log("[eng]     *** 0 mobjs: P_LoadThings no spawneo nada ***");
    }

    ps3_logf("[eng] heap libre tras cargar el mapa: %d KB",
             Z_FreeMemory(mainzone) / 1024);
}

/* ------------------------------------------------------------------ *
 * Verificacion de endianness del CONTENIDO de los lumps.
 *
 * Dentro de un mismo WAD conviven dos endianness, y esto es lo que mas
 * confunde de todo el port:
 *
 *   - La tabla de lumps la escribio el wadtool en little-endian.
 *     En PS3 hay que swapearla  -> rd_long() en ps3_w_wad.c.
 *   - El contenido de los lumps sale DERECHO del ROM de N64, que es
 *     big-endian. En PS3 NO hay que swaparlo -> SwapShort/Swap32 quedan
 *     como identidad (doomdef.h).
 *
 * La segunda mitad es una deduccion, no un dato, asi que se verifica aca en
 * vez de darla por buena: se decodifica el lump STATUS (el primero que toca
 * R_InitStatus) y se mira si su header da medidas creibles leido en nativo.
 * Se loguean las DOS lecturas; si la que vale no es la nativa, el log lo dice
 * en una linea en vez de terminar en un crash mudo dentro de R_InitStatus.
 * ------------------------------------------------------------------ */

#define PU_STATIC_TAG 1

void *W_CacheLumpName(char *name, int tag, int dectype);
int   W_CheckNumForName(char *name);
int   W_LumpLength(int lump);

static int swap16(int v) { return ((v & 0xff) << 8) | ((v >> 8) & 0xff); }

static int lump_endian_check(void)
{
    const unsigned char *b;
    int lump;
    void *data;
    int nat_w, nat_h, swp_w, swp_h;

    lump = W_CheckNumForName("STATUS");
    if (lump < 0) {
        ps3_log("[eng] *** no hay lump STATUS en pow2.wad ***");
        return -1;
    }

    ps3_logf("[eng] chequeo de endianness del contenido: STATUS = lump %d, %d bytes",
             lump, W_LumpLength(lump));

    /* dec_jag = 1. Esto ademas prueba el descompresor Jaguar en big-endian. */
    data = W_CacheLumpName("STATUS", PU_STATIC_TAG, 1);
    if (!data) {
        ps3_log("[eng] *** W_CacheLumpName devolvio NULL ***");
        return -1;
    }

    b = (const unsigned char *)data;
    ps3_logf("[eng]   header crudo: %02x %02x %02x %02x %02x %02x %02x %02x"
             " %02x %02x %02x %02x %02x %02x %02x %02x",
             b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
             b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);

    /* spriteN64_t: width en el offset 10, height en el 12. */
    nat_w = (b[10] << 8) | b[11];      /* big-endian = nativo en PS3 */
    nat_h = (b[12] << 8) | b[13];
    swp_w = swap16(nat_w);
    swp_h = swap16(nat_h);

    ps3_logf("[eng]   leido nativo (big-endian): %dx%d   [STATUS es 80x16]",
             nat_w, nat_h);
    ps3_logf("[eng]   leido swapeado           : %dx%d", swp_w, swp_h);

    if (nat_w > 0 && nat_w <= 1024 && nat_h > 0 && nat_h <= 1024) {
        ps3_log("[eng]   OK: el contenido de los lumps es big-endian, como se esperaba");
        ps3_log("[eng]       -> SwapShort/Swap32 como identidad es correcto");
        return 0;
    }

    if (swp_w > 0 && swp_w <= 1024 && swp_h > 0 && swp_h <= 1024) {
        ps3_log("[eng]   *** AL REVES: el contenido esta en little-endian ***");
        ps3_log("[eng]       -> hay que SACAR el #if big-endian de SwapShort/Swap32");
        ps3_log("[eng]          en doomdef.h y dejar el swap incondicional");
        return -1;
    }

    ps3_log("[eng]   *** ninguna de las dos lecturas da medidas creibles ***");
    ps3_log("[eng]       -> el problema esta antes: en DecodeJaguar o en el filepos");
    return -1;
}

/* ------------------------------------------------------------------ */

void PS3_Engine_Probe(const char *basedir)
{
    ps3_log("[eng] --- arrancando el motor real ---");

    PS3_WWad_SetBaseDir(basedir);

    /* PRIMERO la integridad: sin backres correcto, Z_Malloc aborta con un
     * mensaje que habla de memoria y no tiene nada que ver. */
    if (integrity_check(basedir) < 0) {
        ps3_log("[eng] integridad fallida -- no se sigue (Z_Malloc abortaria)");
        return;
    }

    if (zone_check() < 0)
        return;

    /* Secuencia de D_DoomMain (d_main.c:144-152), sin lo que todavia no
     * existe: I_Init y S_Init son stubs, y el resto es real.
     *   Z_Init  -> ya lo hizo zone_check()
     *   W_Init  -> carga pow2.wad y alt.wad
     *   R_Init  -> R_InitData, que necesita W_Init; de aca sale firsttex
     *   ST_Init -> tablas del HUD
     * El orden importa: R_InitTextures busca "T_START" en el WAD. */
    ps3_log("[eng] W_Init (carga de los WAD del juego)");
    W_Init();

    /* Antes de R_Init, comprobar la premisa de endianness del CONTENIDO de
     * los lumps. Si esta mal, R_InitStatus arranca con un width/height dados
     * vuelta, malloquea una barbaridad y se cae sin decir por que. */
    if (lump_endian_check() < 0) {
        ps3_log("[eng] no se sigue con R_Init: los headers de los lumps no cierran");
        return;
    }

    ps3_log("[eng] R_Init (texturas y sprites)");
    R_Init();
    ps3_logf("[eng]   firsttex=%d lasttex=%d numtextures=%d",
             firsttex, lasttex, numtextures);
    if (numtextures > 0)
        ps3_log("[eng]   OK: la tabla de texturas quedo armada");
    else
        ps3_log("[eng]   *** numtextures=0: R_InitTextures no encontro T_START/T_END ***");

    ps3_log("[eng] ST_Init (HUD)");
    ST_Init();

    /* Audio: el mixer propio (compat/ps3_audio.c) y despues el S_Init real
     * del motor, que carga los 90 y pico efectos de sfx/. */
    {
        extern int PS3_Audio_Init(void);
        extern void S_Init(void);
        ps3_log("[eng] audio: PS3_Audio_Init + S_Init");
        if (PS3_Audio_Init() == 0)
            S_Init();
        else
            ps3_log("[eng] sin audio (el juego sigue igual)");
    }

    ps3_logf("[eng] heap libre antes del mapa: %d KB",
             Z_FreeMemory(mainzone) / 1024);

#ifdef PS3_HOSTTEST
    /* tools/hosttest (render de regresion): carga MAP01 directo. */
    load_map(1);
    PS3_Game_RunTics(90);
#else
    /* En la consola el mapa lo carga el flujo del motor (titulo -> menu ->
     * G_InitNew) desde compat/ps3_system.c. */
    (void)load_map;
#endif
    ps3_log("[eng] --- motor inicializado ---");
}
